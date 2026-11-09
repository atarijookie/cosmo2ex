package main

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/go-chi/chi/v5"
)

// Empty floppy images, ready for the ST to use without formatting them: boot sector, empty FATs and root directory.

const (
	fddSectorSize   = 512
	fddSectorsInFat = 5
	fddRootDirItems = 112
)

// Limited to what makes sense for a new image, and what the geometry detection of .st images (FloppyImageSt::calcParams
// in ce_cores) gets right from the file size - e.g. 84 tracks, 2 sides, 11 sectors has the same size as 77 / 2 / 12.
func validFloppyGeometry(tracks, sectors, sides int) bool {
	return tracks >= 80 && tracks <= 81 && sectors >= 8 && sectors <= 11 && (sides == 1 || sides == 2)
}

// makeEmptyFloppyImage makes the content of the image, the same way as Hatari makes its blank images (createBlankImage.c).
func makeEmptyFloppyImage(tracks, sectors, sides int) ([]byte, error) {
	image := make([]byte, tracks*sectors*sides*fddSectorSize)

	media := byte(0xF8)
	if sides == 2 {
		media = 0xF9
	}

	image[0] = 0xE9 // needed for MS-DOS compatibility
	for i := 2; i < 8; i++ {
		image[i] = 0x4E // loader
	}

	// random serial number - TOS uses it to tell the disks apart, two disks with the same one look like no disk change
	if _, err := rand.Read(image[8:11]); err != nil {
		return nil, err
	}

	binary.LittleEndian.PutUint16(image[11:], fddSectorSize)
	image[13] = 2                                // sectors per cluster
	binary.LittleEndian.PutUint16(image[14:], 1) // reserved sectors: the boot sector
	image[16] = 2                                // FATs
	binary.LittleEndian.PutUint16(image[17:], fddRootDirItems)
	binary.LittleEndian.PutUint16(image[19:], uint16(tracks*sectors*sides))
	image[21] = media
	binary.LittleEndian.PutUint16(image[22:], fddSectorsInFat)
	binary.LittleEndian.PutUint16(image[24:], uint16(sectors))
	binary.LittleEndian.PutUint16(image[26:], uint16(sides))

	for fat := 0; fat < 2; fat++ {
		start := fddSectorSize * (1 + fat*fddSectorsInFat)
		image[start] = media
		image[start+1] = 0xFF
		image[start+2] = 0xFF
	}

	// TOS runs the boot sector when its big endian words add up to 0x1234. With this random serial number it could happen.
	if floppyBootSectorSum(image) == 0x1234 {
		image[510] = 1
	}

	return image, nil
}

func floppyBootSectorSum(image []byte) uint16 {
	sum := uint16(0)
	for i := 0; i < fddSectorSize; i += 2 {
		sum += binary.BigEndian.Uint16(image[i:])
	}
	return sum
}

// newFloppyImagePath makes the path of the new image: name is just a file name, the extension is added if missing.
func newFloppyImagePath(dir, name string) (string, error) {
	name = strings.TrimSpace(name)
	if name == "" || name == "." || name == ".." || strings.ContainsAny(name, `/\:`) {
		return "", errors.New("the file name can't be empty or contain / \\ :")
	}
	if len(name) < 3 || !strings.EqualFold(name[len(name)-3:], ".st") {
		name += ".st"
	}
	if len(name) > 255 {
		return "", errors.New("the file name is too long")
	}

	info, err := os.Stat(dir)
	if err != nil || !info.IsDir() {
		return "", errors.New("the folder doesn't exist: " + dir)
	}
	return filepath.Join(dir, name), nil
}

func (s *Server) handlePostFDDImageNew(w http.ResponseWriter, r *http.Request) {
	logDebugf("handlePostFDDImageNew %s %s", r.Method, r.URL.Path)

	mac := chi.URLParam(r, "mac")

	var body struct {
		Dir     string `json:"dir"`
		Name    string `json:"name"`
		Tracks  int    `json:"tracks"`
		Sectors int    `json:"sectors"`
		Sides   int    `json:"sides"`
		Insert  bool   `json:"insert"`

		Overwrite bool `json:"overwrite"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "invalid request body", http.StatusBadRequest)
		return
	}

	if !validFloppyGeometry(body.Tracks, body.Sectors, body.Sides) {
		http.Error(w, "the geometry must be 80 - 83 tracks, 8 - 11 sectors, 1 - 2 sides", http.StatusBadRequest)
		return
	}

	path, err := newFloppyImagePath(body.Dir, body.Name)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	image, err := makeEmptyFloppyImage(body.Tracks, body.Sectors, body.Sides)
	if err != nil {
		logErrorf("handlePostFDDImageNew - cannot make image: %v", err)
		http.Error(w, "cannot make the image", http.StatusInternalServerError)
		return
	}

	if body.Overwrite {
		err = replaceFile(path, image)
	} else {
		err = writeNewFile(path, image)
	}
	if err != nil {
		if errors.Is(err, fs.ErrExist) { // the page asks if it should be overwritten
			http.Error(w, "this file already exists: "+path, http.StatusConflict)
			return
		}
		logErrorf("handlePostFDDImageNew - cannot write %q: %v", path, err)
		http.Error(w, "cannot write the file: "+path, http.StatusInternalServerError)
		return
	}

	current, _ := s.readFddImage(mac)
	replacedInserted := current != "" && filepath.Clean(current) == filepath.Clean(path)

	if body.Insert && !replacedInserted {
		if err := s.writeFddImage(mac, path); err != nil {
			http.Error(w, "the image was created, but it can't be inserted: cannot write FLOPPY_IMAGE", http.StatusInternalServerError)
			return
		}
	}

	// the inserted image was replaced: inserted again in any case, the ST must not go on with the old content
	if body.Insert || replacedInserted {
		if err := handleFloppyAction("insert", mac); err != nil {
			logErrorf("handlePostFDDImageNew - failed to send insert command: %v", err)
		}
	}

	writeJSON(w, http.StatusOK, map[string]string{"mac": mac, "image": path})
}

// replaceFile swaps the whole file at once, so the core can see it's another file (and it can't get half of it). A symlink
// stays and the file it points to is replaced, the access rights stay too.
func replaceFile(path string, data []byte) error {
	target := path
	if resolved, err := filepath.EvalSymlinks(path); err == nil {
		target = resolved
	}

	mode := os.FileMode(0o644)
	info, err := os.Stat(target)
	if err == nil {
		if !info.Mode().IsRegular() {
			return errors.New("not a regular file")
		}
		mode = info.Mode().Perm()
	} else if !os.IsNotExist(err) {
		return err
	}

	temp, err := os.CreateTemp(filepath.Dir(target), ".new_floppy_*")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name()) // does nothing after the rename

	_, err = temp.Write(data)
	if closeErr := temp.Close(); err == nil {
		err = closeErr
	}
	if err == nil {
		err = os.Chmod(temp.Name(), mode)
	}
	if err == nil {
		err = os.Rename(temp.Name(), target)
	}
	return err
}

// writeNewFile never overwrites a file, and leaves no partial file behind on failure.
func writeNewFile(path string, data []byte) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return err
	}

	_, err = file.Write(data)
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		os.Remove(path)
	}
	return err
}
