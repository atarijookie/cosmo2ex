package main

import (
	"archive/zip"
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// Downloader of floppy images from a list (FLOPPY_DOWNLOAD_URL in the general settings) into FLOPPY_IMAGES_DIR - when the
// user allowed it, a while after the start and then once a day. Images already there are not downloaded again; for a
// zip (unpacked, only the .st / .msa in it are kept) an empty file with its name stays as the mark that it's done, as the
// images in it can have any names. Existing images are never overwritten - the ST may have written to them.
//
// List format: a version line (date), then one line per file: URL, checksum (not used, 0x0000), names of the games on
// the disk. Other lines are skipped.

const (
	floppyDownloadStartDelay = 10 * time.Second
	floppyDownloadInterval   = 24 * time.Hour
	floppyDownloadPause      = 1 * time.Second    // between the downloaded files - be nice to the servers
	floppyDownloadTimeout    = 2 * time.Minute    // one file (or the list)
	floppyFailedRetryAfter   = 7 * 24 * time.Hour // a file that's not there (anymore), or not an image - don't ask every day
	floppyMaxNetworkErrors   = 5                  // that many in a row - the network is down, end the run
	floppyFreeSpaceReserve   = 1 << 30            // keep this much free on the disk
	floppyMaxDownloadSize    = 32 << 20           // one zip / image file
	floppyMaxImageSize       = 4 << 20            // one unpacked image - HD floppy MSA / ST is below 2 MB
	floppyMaxListSize        = 16 << 20
)

var (
	errNoSpace  = errors.New("not enough free space")
	errBadImage = errors.New("not a floppy image")
)

type floppyListEntry struct {
	URL    string
	Name   string   // file name from the URL
	Folder string   // subfolder of FLOPPY_IMAGES_DIR from the list - not in the list format yet, "" = the dir itself
	Games  []string // names of the games on the disk, for the web UI later
}

// parseFloppyImageList reads the list, skips the lines which aren't a valid entry (counted in skipped)
func parseFloppyImageList(r io.Reader) (version string, entries []floppyListEntry, skipped int, err error) {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)

	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}

		fields := strings.Split(line, ",")
		e, ok := floppyListEntryFromFields(fields)
		if !ok {
			if version == "" && len(entries) == 0 && len(fields) == 1 { // the first line - list version
				version = line
			} else {
				skipped++
			}
			continue
		}
		entries = append(entries, e)
	}
	return version, entries, skipped, sc.Err()
}

func floppyListEntryFromFields(fields []string) (floppyListEntry, bool) {
	rawURL := strings.TrimSpace(fields[0])
	u, err := url.Parse(rawURL)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		return floppyListEntry{}, false
	}

	name := path.Base(u.Path)
	if !safeFloppyFileName(name) || !(isFloppyImageName(name) || strings.EqualFold(path.Ext(name), ".zip")) {
		return floppyListEntry{}, false
	}

	e := floppyListEntry{URL: rawURL, Name: name}
	if len(fields) > 2 {
		for _, g := range fields[2:] {
			if g = strings.TrimSpace(g); g != "" {
				e.Games = append(e.Games, g)
			}
		}
	}
	return e, true
}

// safeFloppyFileName - just a file name: no dirs, not hidden (our temp files are), not too long for the device
func safeFloppyFileName(name string) bool {
	return name != "" && name != "." && name != ".." && !strings.HasPrefix(name, ".") && len(name) <= 255 &&
		!strings.ContainsAny(name, "/\\:")
}

// safeFloppySubfolder - relative, without ".." (comes from the list)
func safeFloppySubfolder(folder string) bool {
	if folder == "" {
		return true
	}
	if filepath.IsAbs(folder) || strings.ContainsAny(folder, ":\\") {
		return false
	}
	for _, part := range strings.Split(folder, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

type floppyDownloadStatus struct {
	State       string `json:"state"` // waiting / running / done / disabled / no-space / error
	Message     string `json:"message"`
	ListVersion string `json:"listVersion"`
	Total       int    `json:"total"`
	Checked     int    `json:"checked"`
	Present     int    `json:"present"` // there before this run
	Downloaded  int    `json:"downloaded"`
	Failed      int    `json:"failed"`
	Current     string `json:"current"`
	LastStart   string `json:"lastStart"`
	LastEnd     string `json:"lastEnd"`
	NextRun     string `json:"nextRun"`
	FreeBytes   uint64 `json:"freeBytes"`
	MinFree     uint64 `json:"minFree"`
}

type floppyDownloader struct {
	mu     sync.Mutex
	status floppyDownloadStatus
	failed map[string]time.Time // URL -> when it failed for good (not found, not an image)
	wake   chan struct{}

	client   *http.Client
	settings func() generalSettings
	dir      func() string
	diskFree func(string) (uint64, error)
	pause    time.Duration
}

func newFloppyDownloader() *floppyDownloader {
	return &floppyDownloader{
		status:   floppyDownloadStatus{State: "waiting", MinFree: floppyFreeSpaceReserve},
		failed:   map[string]time.Time{},
		wake:     make(chan struct{}, 1),
		client:   &http.Client{Timeout: floppyDownloadTimeout},
		settings: readGeneralSettings,
		dir:      floppyImagesDir,
		diskFree: diskFreeBytes,
		pause:    floppyDownloadPause,
	}
}

// start runs the downloader in the background: a while after the start, then once a day, or when trigger() is called
func (d *floppyDownloader) start() {
	go func() {
		timer := time.NewTimer(floppyDownloadStartDelay)
		d.setNextRun(time.Now().Add(floppyDownloadStartDelay))

		for {
			select {
			case <-timer.C:
			case <-d.wake:
				if !timer.Stop() {
					<-timer.C
				}
			}

			d.run()

			timer.Reset(floppyDownloadInterval)
			d.setNextRun(time.Now().Add(floppyDownloadInterval))
		}
	}()
}

// trigger - run now (e.g. downloading was just allowed); while running, it runs once more after that
func (d *floppyDownloader) trigger() {
	select {
	case d.wake <- struct{}{}:
	default:
	}
}

func (d *floppyDownloader) getStatus() floppyDownloadStatus {
	d.mu.Lock()
	defer d.mu.Unlock()

	st := d.status
	if dir := d.dir(); dir != "" && st.State != "running" { // fresh value for the settings page
		if free, err := d.diskFree(existingParent(dir)); err == nil {
			st.FreeBytes = free
		}
	}
	return st
}

func (d *floppyDownloader) setNextRun(t time.Time) {
	d.mu.Lock()
	d.status.NextRun = t.Format(time.RFC3339)
	d.mu.Unlock()
}

func (d *floppyDownloader) update(f func(st *floppyDownloadStatus)) {
	d.mu.Lock()
	f(&d.status)
	d.mu.Unlock()
}

func (d *floppyDownloader) finish(state, msg string) {
	log.Printf("floppy download - %s: %s", state, msg)
	d.update(func(st *floppyDownloadStatus) {
		st.State = state
		st.Message = msg
		st.Current = ""
		st.LastEnd = time.Now().Format(time.RFC3339)
	})
}

// existingParent - dir, or its closest parent which exists (for the free space before the dir is created)
func existingParent(dir string) string {
	for {
		if _, err := os.Stat(dir); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return dir
		}
		dir = parent
	}
}

// enoughSpace - can one more file be downloaded, and still keep floppyFreeSpaceReserve free?
func (d *floppyDownloader) enoughSpace(dir string) bool {
	free, err := d.diskFree(dir)
	if err != nil {
		log.Printf("floppy download - can't get free space of %s: %v", dir, err)
		return false
	}
	d.update(func(st *floppyDownloadStatus) { st.FreeBytes = free })
	return free >= floppyFreeSpaceReserve+floppyMaxDownloadSize+floppyMaxImageSize
}

// run goes through the list once
func (d *floppyDownloader) run() {
	gs := d.settings()
	dir := d.dir()

	d.update(func(st *floppyDownloadStatus) {
		*st = floppyDownloadStatus{State: "running", LastStart: time.Now().Format(time.RFC3339), NextRun: st.NextRun,
			MinFree: floppyFreeSpaceReserve, FreeBytes: st.FreeBytes}
	})

	if !gs.FloppyDownloadEnabled {
		d.finish("disabled", "downloading of floppy images is not allowed in the settings")
		return
	}
	if dir == "" {
		d.finish("error", "FLOPPY_IMAGES_DIR is not set in .env")
		return
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		d.finish("error", fmt.Sprintf("can't create %s: %v", dir, err))
		return
	}
	if !d.enoughSpace(dir) {
		d.finish("no-space", fmt.Sprintf("less than %d MB would stay free in %s", floppyFreeSpaceReserve>>20, dir))
		return
	}

	version, entries, skipped, err := d.fetchList(gs.FloppyDownloadURL)
	if err != nil {
		d.finish("error", fmt.Sprintf("can't get the list %s: %v", gs.FloppyDownloadURL, err))
		return
	}
	log.Printf("floppy download - list %s: version %q, %d files, %d lines skipped", gs.FloppyDownloadURL, version, len(entries), skipped)
	d.update(func(st *floppyDownloadStatus) {
		st.ListVersion = version
		st.Total = len(entries)
	})

	networkErrors := 0

	for i, e := range entries {
		if !d.settings().FloppyDownloadEnabled { // not allowed anymore? stop right away
			d.finish("disabled", "stopped - downloading of floppy images is not allowed anymore")
			return
		}

		d.update(func(st *floppyDownloadStatus) { st.Current = e.Name })
		result, err := d.handleEntry(dir, e)

		switch {
		case errors.Is(err, errNoSpace):
			d.finish("no-space", fmt.Sprintf("stopped - less than %d MB would stay free in %s", floppyFreeSpaceReserve>>20, dir))
			return
		case err != nil:
			log.Printf("floppy download - %s: %v", e.URL, err)
			var netErr *floppyNetworkError
			if errors.As(err, &netErr) {
				networkErrors++
			} else {
				networkErrors = 0
			}
		default:
			networkErrors = 0
		}

		d.update(func(st *floppyDownloadStatus) {
			st.Checked++
			switch result {
			case floppyPresent:
				st.Present++
			case floppyDownloaded:
				st.Downloaded++
			case floppyFailed, floppyFailedBefore:
				st.Failed++
			}
		})

		if networkErrors >= floppyMaxNetworkErrors {
			d.finish("error", fmt.Sprintf("stopped - %d network errors in a row, last: %v", networkErrors, err))
			return
		}

		if (result == floppyDownloaded || result == floppyFailed) && i < len(entries)-1 { // asked the server - pause before the next one
			time.Sleep(d.pause)
		}
	}

	st := d.getStatus()
	d.finish("done", fmt.Sprintf("%d downloaded, %d were there, %d failed", st.Downloaded, st.Present, st.Failed))
}

type floppyEntryResult int

const (
	floppyPresent floppyEntryResult = iota
	floppyDownloaded
	floppyFailed
	floppyFailedBefore // failed on an earlier run, not tried again yet
)

// floppyNetworkError - couldn't talk to the server (or it had trouble), try again on the next run
type floppyNetworkError struct{ err error }

func (e *floppyNetworkError) Error() string { return e.err.Error() }
func (e *floppyNetworkError) Unwrap() error { return e.err }

func (d *floppyDownloader) handleEntry(dir string, e floppyListEntry) (floppyEntryResult, error) {
	if !safeFloppySubfolder(e.Folder) {
		return floppyFailed, fmt.Errorf("bad folder %q", e.Folder)
	}
	folder := filepath.Join(dir, filepath.FromSlash(e.Folder))
	target := filepath.Join(folder, e.Name) // the image, or the mark of an unpacked zip

	if _, err := os.Lstat(target); err == nil {
		return floppyPresent, nil
	}

	d.mu.Lock()
	failedAt, failed := d.failed[e.URL]
	d.mu.Unlock()
	if failed && time.Since(failedAt) < floppyFailedRetryAfter { // logged when it failed, not asked for until then
		return floppyFailedBefore, nil
	}

	if !d.enoughSpace(existingParent(folder)) {
		return floppyFailed, errNoSpace
	}
	if err := os.MkdirAll(folder, 0o755); err != nil {
		return floppyFailed, err
	}

	err := d.downloadEntry(folder, target, e)
	if err == nil {
		return floppyDownloaded, nil
	}

	var netErr *floppyNetworkError
	if !errors.As(err, &netErr) { // the file is wrong, not the network - don't download it every day
		d.mu.Lock()
		d.failed[e.URL] = time.Now()
		d.mu.Unlock()
	}
	return floppyFailed, err
}

func (d *floppyDownloader) downloadEntry(folder, target string, e floppyListEntry) error {
	tmp := filepath.Join(folder, "."+e.Name+".part") // hidden and not .st / .msa - not listed by ce_cores
	defer os.Remove(tmp)

	if err := d.download(e.URL, tmp, floppyMaxDownloadSize); err != nil {
		return err
	}

	if !strings.EqualFold(path.Ext(e.Name), ".zip") { // plain image
		if err := checkFloppyImageFile(tmp, e.Name); err != nil {
			return err
		}
		return renameIfMissing(tmp, target)
	}

	images, err := extractFloppyZip(tmp, folder)
	if err != nil {
		return err
	}
	log.Printf("floppy download - %s: %d images %v", e.Name, len(images), images)

	return os.WriteFile(target, nil, 0o644) // empty zip - the mark that this one is done
}

// download url into file, at most maxSize bytes
func (d *floppyDownloader) download(rawURL, file string, maxSize int64) error {
	ctx, cancel := context.WithTimeout(context.Background(), floppyDownloadTimeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "cosmo2ex-webserver (floppy image downloader)")

	resp, err := d.client.Do(req)
	if err != nil {
		return &floppyNetworkError{err}
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		err := fmt.Errorf("HTTP %s", resp.Status)
		if resp.StatusCode >= 500 || resp.StatusCode == http.StatusTooManyRequests { // server trouble - maybe ok tomorrow
			return &floppyNetworkError{err}
		}
		return err
	}

	f, err := os.Create(file)
	if err != nil {
		return err
	}
	n, copyErr := io.Copy(f, io.LimitReader(resp.Body, maxSize+1))
	closeErr := f.Close()

	if copyErr != nil {
		return &floppyNetworkError{copyErr}
	}
	if closeErr != nil {
		return closeErr
	}
	if n > maxSize {
		return fmt.Errorf("bigger than %d MB", maxSize>>20)
	}
	return nil
}

func (d *floppyDownloader) fetchList(listURL string) (string, []floppyListEntry, int, error) {
	tmp, err := os.CreateTemp("", "cosmo2ex-imagelist-*.csv")
	if err != nil {
		return "", nil, 0, err
	}
	tmpName := tmp.Name()
	tmp.Close()
	defer os.Remove(tmpName)

	if err := d.download(listURL, tmpName, floppyMaxListSize); err != nil {
		return "", nil, 0, err
	}

	f, err := os.Open(tmpName)
	if err != nil {
		return "", nil, 0, err
	}
	defer f.Close()
	return parseFloppyImageList(f)
}

// extractFloppyZip unpacks the .st / .msa files from the zip into folder (without the dirs in the zip), returns their names.
// The images which are there already are kept - they may have the ST's writes in them.
func extractFloppyZip(zipFile, folder string) ([]string, error) {
	zr, err := zip.OpenReader(zipFile)
	if err != nil {
		return nil, fmt.Errorf("not a zip file: %v", err)
	}
	defer zr.Close()

	var images []string
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		name := path.Base(strings.ReplaceAll(f.Name, "\\", "/"))
		if !safeFloppyFileName(name) || !isFloppyImageName(name) {
			continue
		}
		if f.UncompressedSize64 > floppyMaxImageSize {
			log.Printf("floppy download - %s in %s: too big (%d B), skipped", f.Name, zipFile, f.UncompressedSize64)
			continue
		}

		target := filepath.Join(folder, name)
		if _, err := os.Lstat(target); err == nil {
			continue
		}

		if err := extractZipEntry(f, target); err != nil {
			if errors.Is(err, errBadImage) { // just this one is wrong - the others can be fine
				log.Printf("floppy download - %s in %s: %v, skipped", f.Name, zipFile, err)
				continue
			}
			return images, fmt.Errorf("%s: %v", f.Name, err)
		}
		images = append(images, name)
	}
	return images, nil
}

func extractZipEntry(f *zip.File, target string) error {
	rc, err := f.Open()
	if err != nil {
		return err
	}
	defer rc.Close()

	tmp := filepath.Join(filepath.Dir(target), "."+filepath.Base(target)+".part")
	defer os.Remove(tmp)

	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	n, copyErr := io.Copy(out, io.LimitReader(rc, floppyMaxImageSize+1)) // the size in the zip could lie
	closeErr := out.Close()
	if copyErr != nil {
		return copyErr
	}
	if closeErr != nil {
		return closeErr
	}
	if n > floppyMaxImageSize {
		return fmt.Errorf("%w - bigger than %d MB", errBadImage, floppyMaxImageSize>>20)
	}
	if err := checkFloppyImageFile(tmp, filepath.Base(target)); err != nil {
		return err
	}
	return renameIfMissing(tmp, target)
}

// checkFloppyImageFile - looks like the image its name says, not e.g. an error page
func checkFloppyImageFile(file, name string) error {
	data, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	if strings.EqualFold(path.Ext(name), ".msa") {
		if len(data) < 10 || data[0] != 0x0e || data[1] != 0x0f {
			return fmt.Errorf("%w - no MSA header", errBadImage)
		}
		return nil
	}
	if len(data) == 0 || len(data)%512 != 0 { // .st - raw sectors
		return fmt.Errorf("%w - ST size is not whole sectors", errBadImage)
	}
	return nil
}

// renameIfMissing moves tmp to target, but never over an existing file
func renameIfMissing(tmp, target string) error {
	if _, err := os.Lstat(target); err == nil {
		return nil
	}
	return os.Rename(tmp, target)
}
