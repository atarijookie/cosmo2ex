package main

import (
	"bytes"
	"crypto/md5"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

// Firmware update (OTA) of the HDD / FDD devices from the web UI (firmware_update.html).
//
// The user downloads the .uf2 from the GitHub releases and drops it on the page. The webserver extracts the flash image
// from it, checks it against what the device reported about its running firmware (FW_* files in its settings dir,
// written by ce_cores - ChipInterface::storeFwInfo()), stores it as FW_UPDATE_BIN in the device's settings dir and asks
// ce_cores to send it to the device (FwUpdateSender). ce_cores writes the progress to FW_UPDATE_STATUS.
// The device checks the image once more (ce_pico_hdd/fw_update.cpp, ce_pico_fdd/fw_update.cpp) before it reboots into
// it. The new firmware is on trial until it connects to the host; when it doesn't, the device goes back to the previous
// firmware after 3 boots and reports FW_STATE "rolled_back".

const (
	fwReleasesURL = "https://github.com/atarijookie/cosmo2ex/releases"

	fwUpdateBinKey    = "FW_UPDATE_BIN"
	fwUpdateStatusKey = "FW_UPDATE_STATUS"

	fwMaxUF2Size  = 16 << 20
	fwMaxBinSize  = 4 << 20
	fwFlashBase   = 0x10000000
	fwLayoutOff   = 0x2ff0   // FS start, FS end, EEPROM start, flash length (arduino-pico memmap_default.ld, .partition)
	fwFsReserve   = 16 << 10 // LittleFS needs some space for itself (the same as in fw_update.cpp)
	fwIdentPrefix = "CEFWID:"

	uf2Magic0   = 0x0A324655
	uf2Magic1   = 0x9E5D5157
	uf2MagicEnd = 0x0AB16F30

	uf2FlagNotMainFlash = 0x00000001
	uf2FlagFamilyID     = 0x00002000

	uf2FamilyRP2040      = 0xe48bff56
	uf2FamilyAbsolute    = 0xe48bff57 // RP2350-E10 workaround block from picotool - not part of the image
	uf2FamilyRP2350ARM   = 0xe48bff59
	uf2FamilyRP2350RISCV = 0xe48bff5a
)

// an update in one of these states is still running (if its status isn't too old - ce_cores might have been restarted)
var fwRunningStates = map[string]bool{"requested": true, "starting": true, "sending": true, "finishing": true}

const fwRunningStateMaxAge = 60 * time.Second

//--------------------------------------------------------
// UF2

type uf2Image struct {
	Chip   string // RP2040, RP2350
	Family uint32
	Data   []byte // flash image from fwFlashBase
}

func uf2FamilyChip(family uint32) string {
	switch family {
	case uf2FamilyRP2040:
		return "RP2040"
	case uf2FamilyRP2350ARM, uf2FamilyRP2350RISCV:
		return "RP2350"
	}
	return ""
}

// parseUF2 returns the contiguous flash image from a UF2 file of RP2040 / RP2350.
func parseUF2(b []byte) (*uf2Image, error) {
	if len(b) == 0 || len(b)%512 != 0 {
		return nil, errors.New("not a UF2 file (size is not a multiple of 512 bytes)")
	}

	type block struct {
		addr uint32
		data []byte
	}

	var blocks []block
	family := uint32(0)

	for off := 0; off < len(b); off += 512 {
		blk := b[off : off+512]
		le := binary.LittleEndian

		if le.Uint32(blk[0:]) != uf2Magic0 || le.Uint32(blk[4:]) != uf2Magic1 || le.Uint32(blk[508:]) != uf2MagicEnd {
			return nil, errors.New("not a UF2 file (bad block signature)")
		}

		flags := le.Uint32(blk[8:])
		if flags&uf2FlagNotMainFlash != 0 {
			continue
		}
		if flags&uf2FlagFamilyID == 0 {
			return nil, errors.New("UF2 file without chip family")
		}

		fam := le.Uint32(blk[28:])
		if fam == uf2FamilyAbsolute {
			continue
		}
		if uf2FamilyChip(fam) == "" {
			return nil, fmt.Errorf("UF2 file is not for RP2040 / RP2350 (family 0x%08x)", fam)
		}
		if family != 0 && fam != family {
			return nil, errors.New("UF2 file contains images for more chips")
		}
		family = fam

		size := le.Uint32(blk[16:])
		if size > 476 {
			return nil, errors.New("UF2 file has a bad block size")
		}

		blocks = append(blocks, block{addr: le.Uint32(blk[12:]), data: blk[32 : 32+size]})
	}

	if len(blocks) == 0 {
		return nil, errors.New("UF2 file contains no flash data")
	}

	sort.Slice(blocks, func(i, j int) bool { return blocks[i].addr < blocks[j].addr })

	if blocks[0].addr != fwFlashBase {
		return nil, fmt.Errorf("UF2 image doesn't start at the start of flash (0x%08x)", blocks[0].addr)
	}

	var data []byte
	next := uint32(fwFlashBase)
	for _, bl := range blocks {
		if bl.addr != next {
			return nil, fmt.Errorf("UF2 image is not contiguous (gap or overlap at 0x%08x)", bl.addr)
		}
		data = append(data, bl.data...)
		next += uint32(len(bl.data))

		if len(data) > fwMaxBinSize {
			return nil, errors.New("UF2 image is too big")
		}
	}

	return &uf2Image{Chip: uf2FamilyChip(family), Family: family, Data: data}, nil
}

// fwIdentOf finds the identity string in the image: "CEFWID:dev=HDD;type=W6100-EVB-PICO;chip=RP2040;build=v1.0.0;"
func fwIdentOf(bin []byte) map[string]string {
	i := bytes.Index(bin, []byte(fwIdentPrefix))
	if i < 0 {
		return nil
	}

	rest := bin[i+len(fwIdentPrefix):]
	end := bytes.IndexByte(rest, 0)
	if end < 0 || end > 256 {
		return nil
	}

	return parseFwIdent(string(rest[:end]))
}

func parseFwIdent(s string) map[string]string {
	m := map[string]string{}
	for _, part := range strings.Split(s, ";") {
		if k, v, ok := strings.Cut(part, "="); ok {
			m[strings.TrimSpace(k)] = strings.TrimSpace(v)
		}
	}
	if m["dev"] == "" || m["type"] == "" {
		return nil
	}
	return m
}

// fwFileName is the release file this device needs, e.g. ce_pico_hdd_W6100-EVB-PICO.uf2
func fwFileName(dev, devType string) string {
	if dev == "" || devType == "" {
		return ""
	}
	return "ce_pico_" + strings.ToLower(dev) + "_" + devType + ".uf2"
}

func md5Hex(b []byte) string {
	sum := md5.Sum(b)
	return hex.EncodeToString(sum[:])
}

//--------------------------------------------------------
// what the device reported

type fwDeviceInfo struct {
	Connected   bool   `json:"connected"`
	Reported    bool   `json:"reported"` // the device sent its firmware info - it supports the update
	Dev         string `json:"dev"`      // HDD, FDD
	Type        string `json:"type"`     // W6100-EVB-PICO, PICO-2W, ...
	Chip        string `json:"chip"`
	Build       string `json:"build"`
	Version     string `json:"version"`
	ImageSize   int64  `json:"image_size"`
	ImageMD5    string `json:"image_md5"`
	FsStart     int64  `json:"fs_start"`
	FsEnd       int64  `json:"fs_end"`
	OTA         bool   `json:"ota"`   // built with the file system - the update is possible
	State       string `json:"state"` // after an update: "trial" (new firmware not confirmed yet), "rolled_back", "ok"
	NeededFile  string `json:"needed_file"`
	ReleasesURL string `json:"releases_url"`

	Update      json.RawMessage `json:"update"`       // FW_UPDATE_STATUS as written by ce_cores (null if none)
	PendingFile *fwFileInfo     `json:"pending_file"` // the checked image waiting / used for the update
	PendingRuns bool            `json:"pending_runs"` // the device runs the pending image now - the update is done
}

type fwFileInfo struct {
	Dev   string `json:"dev"`
	Type  string `json:"type"`
	Chip  string `json:"chip"`
	Build string `json:"build"`
	Size  int    `json:"size"`
	MD5   string `json:"md5"`
}

func fwDeviceDir(mac string) string {
	return filepath.Join(getSettingsDir(), mac)
}

func readFwDeviceInfo(mac string) fwDeviceInfo {
	dir := fwDeviceDir(mac)

	info := fwDeviceInfo{
		Connected:   connectedMACs()[mac],
		ReleasesURL: fwReleasesURL,
		Version:     readSettingString(dir, "FW_VERSION"),
		ImageMD5:    strings.ToLower(readSettingString(dir, "FW_IMAGE_MD5")),
		OTA:         readSettingString(dir, "FW_OTA") == "1",
		State:       readSettingString(dir, "FW_STATE"),
	}

	info.ImageSize, _ = strconv.ParseInt(readSettingString(dir, "FW_IMAGE_SIZE"), 10, 64)
	info.FsStart, _ = strconv.ParseInt(readSettingString(dir, "FW_FS_START"), 10, 64)
	info.FsEnd, _ = strconv.ParseInt(readSettingString(dir, "FW_FS_END"), 10, 64)

	if ident := parseFwIdent(readSettingString(dir, "FW_IDENT")); ident != nil {
		info.Reported = true
		info.Dev = ident["dev"]
		info.Type = ident["type"]
		info.Chip = ident["chip"]
		info.Build = ident["build"]
		info.NeededFile = fwFileName(info.Dev, info.Type)
	}

	if st, err := os.ReadFile(filepath.Join(dir, fwUpdateStatusKey)); err == nil && json.Valid(bytes.TrimSpace(st)) {
		info.Update = json.RawMessage(bytes.TrimSpace(st))
	}

	if bin, err := os.ReadFile(filepath.Join(dir, fwUpdateBinKey)); err == nil {
		info.PendingFile = fwFileInfoOf(bin, "")
		info.PendingRuns = fwRunsImage(info, bin)
	}

	return info
}

func fwFileInfoOf(bin []byte, chip string) *fwFileInfo {
	fi := &fwFileInfo{Size: len(bin), MD5: md5Hex(bin), Chip: chip}
	if ident := fwIdentOf(bin); ident != nil {
		fi.Dev, fi.Type, fi.Build = ident["dev"], ident["type"], ident["build"]
		if fi.Chip == "" {
			fi.Chip = ident["chip"]
		}
	}
	return fi
}

// fwRunsImage - the device runs this image: the device hashes its image without the padding of the last UF2 block
func fwRunsImage(info fwDeviceInfo, bin []byte) bool {
	n := info.ImageSize
	if n <= 0 || info.ImageMD5 == "" || int64(len(bin)) < n || int64(len(bin))-n >= 256 {
		return false
	}
	return md5Hex(bin[:n]) == info.ImageMD5
}

// fwUpdateRunning - an update is in progress (and its status is fresh)
func fwUpdateRunning(info fwDeviceInfo) bool {
	if info.Update == nil {
		return false
	}

	var st struct {
		State string `json:"state"`
		Time  int64  `json:"time"`
	}
	if json.Unmarshal(info.Update, &st) != nil {
		return false
	}

	return fwRunningStates[st.State] && time.Since(time.Unix(st.Time, 0)) < fwRunningStateMaxAge
}

// checkFirmware returns the reasons why this image can't go into the device (empty = it can).
// uf2Chip - chip of the UF2 file ("" = not known, e.g. the stored image).
func checkFirmware(info fwDeviceInfo, bin []byte, uf2Chip string) []string {
	var problems []string

	if !info.Connected {
		problems = append(problems, "The device is not connected.")
	}

	if !info.Reported {
		problems = append(problems, "The device didn't report its firmware details - its firmware can't be updated from here. Update it once via USB.")
		return problems
	}

	if !info.OTA {
		problems = append(problems, "The firmware in the device was built without the file system needed for the update. Update it once via USB.")
		return problems
	}

	if uf2Chip != "" && info.Chip != "" && uf2Chip != info.Chip {
		problems = append(problems, fmt.Sprintf("This file is for the %s chip, the device has %s - you need %s.", uf2Chip, info.Chip, info.NeededFile))
		return problems
	}

	ident := fwIdentOf(bin)
	if ident == nil {
		problems = append(problems, "This is not a CosmosEx firmware (or it's too old - no identity found in it).")
		return problems
	}

	if ident["dev"] != info.Dev || ident["type"] != info.Type {
		problems = append(problems, fmt.Sprintf("This firmware is for %s %s, but this device is %s %s - you need %s.",
			ident["dev"], ident["type"], info.Dev, info.Type, info.NeededFile))
		return problems
	}

	if len(bin) < fwLayoutOff+16 {
		problems = append(problems, "The firmware image is too short.")
		return problems
	}

	fsStart := int64(binary.LittleEndian.Uint32(bin[fwLayoutOff:])) - fwFlashBase
	fsEnd := int64(binary.LittleEndian.Uint32(bin[fwLayoutOff+4:])) - fwFlashBase
	if fsStart != info.FsStart || fsEnd != info.FsEnd {
		problems = append(problems, "This firmware has a different flash layout (flash size / file system setting) than the one in the device. Update it via USB.")
		return problems
	}

	if int64(len(bin)) > info.FsStart || int64(len(bin)) > info.FsEnd-info.FsStart-fwFsReserve {
		problems = append(problems, fmt.Sprintf("The firmware is too big for the update (%d bytes).", len(bin)))
	}

	if fwRunsImage(info, bin) {
		problems = append(problems, "This firmware is already running in the device.")
	}

	return problems
}

//--------------------------------------------------------
// handlers

// fwMacParam - the device MAC from the URL as the name of its settings dir (12 upper case hex chars), "" if invalid
func fwMacParam(r *http.Request) string {
	mac, err := url.PathUnescape(chi.URLParam(r, "mac"))
	if err != nil {
		return ""
	}

	mac = strings.ToUpper(stripMACColons(mac))
	if !isValidMACFormat(mac) || mac == "000000000000" {
		return ""
	}
	return mac
}

func (s *Server) handleGetFirmware(w http.ResponseWriter, r *http.Request) {
	mac := fwMacParam(r)
	if mac == "" {
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	writeJSON(w, http.StatusOK, readFwDeviceInfo(mac))
}

type fwCheckResult struct {
	OK       bool        `json:"ok"`
	Problems []string    `json:"problems"`
	File     *fwFileInfo `json:"file"`
}

// handlePostFirmwareCheck - body: the .uf2 file. When it can go into the device, it's stored for the update.
func (s *Server) handlePostFirmwareCheck(w http.ResponseWriter, r *http.Request) {
	mac := fwMacParam(r)
	if mac == "" {
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, fwMaxUF2Size))
	if err != nil {
		http.Error(w, "file too big or upload failed", http.StatusRequestEntityTooLarge)
		return
	}

	info := readFwDeviceInfo(mac)
	res := fwCheckResult{Problems: []string{}}

	img, err := parseUF2(body)
	if err != nil {
		res.Problems = append(res.Problems, "This is not a valid firmware file: "+err.Error()+".")
		writeJSON(w, http.StatusOK, res)
		return
	}

	res.File = fwFileInfoOf(img.Data, img.Chip)
	res.Problems = append(res.Problems, checkFirmware(info, img.Data, img.Chip)...)

	if fwUpdateRunning(info) {
		res.Problems = append(res.Problems, "An update of this device is running now.")
	}

	if len(res.Problems) == 0 {
		if err := writeFileAtomically(filepath.Join(fwDeviceDir(mac), fwUpdateBinKey), img.Data); err != nil {
			logErrorf("handlePostFirmwareCheck - cannot store the image: %v", err)
			http.Error(w, "cannot store the firmware", http.StatusInternalServerError)
			return
		}
		res.OK = true
	}

	logInfof("handlePostFirmwareCheck - %s: %s %s build %s, %d B, ok: %v %v", mac, res.File.Dev, res.File.Type, res.File.Build, res.File.Size, res.OK, res.Problems)
	writeJSON(w, http.StatusOK, res)
}

// handlePostFirmwareUpdate - send the stored (checked) image to the device
func (s *Server) handlePostFirmwareUpdate(w http.ResponseWriter, r *http.Request) {
	mac := fwMacParam(r)
	if mac == "" {
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	dir := fwDeviceDir(mac)
	bin, err := os.ReadFile(filepath.Join(dir, fwUpdateBinKey))
	if err != nil {
		http.Error(w, "no checked firmware - choose the file again", http.StatusConflict)
		return
	}

	info := readFwDeviceInfo(mac)
	if fwUpdateRunning(info) {
		http.Error(w, "An update of this device is running now.", http.StatusConflict)
		return
	}

	if problems := checkFirmware(info, bin, ""); len(problems) > 0 { // the device might have changed since the check
		http.Error(w, strings.Join(problems, " "), http.StatusConflict)
		return
	}

	// ce_cores overwrites this as soon as it starts - until then the page knows the request is on its way
	status, _ := json.Marshal(map[string]interface{}{"state": "requested", "progress": 0, "sent": 0, "total": len(bin), "message": "", "time": time.Now().Unix()})
	if err := writeFileAtomically(filepath.Join(dir, fwUpdateStatusKey), status); err != nil {
		logErrorf("handlePostFirmwareUpdate - cannot write status: %v", err)
	}

	os.Remove(filepath.Join(dir, "FW_STATE")) // trial / rolled back of an earlier update - the device reports it again after this one

	cmd, _ := json.Marshal(map[string]string{"module": "all", "action": "fw_update", "dev": strings.ToLower(info.Dev), "mac": mac, "md5": md5Hex(bin)})
	if err := sendToCmdSocket(string(cmd)); err != nil {
		logErrorf("handlePostFirmwareUpdate - failed to send cmd socket: %v", err)
		http.Error(w, "cannot reach the core (ce_cores)", http.StatusBadGateway)
		return
	}

	logInfof("handlePostFirmwareUpdate - %s: update requested, %d B", mac, len(bin))
	w.WriteHeader(http.StatusAccepted)
}

func (s *Server) handlePostFirmwareAbort(w http.ResponseWriter, r *http.Request) {
	mac := fwMacParam(r)
	if mac == "" {
		http.Error(w, "invalid MAC address", http.StatusBadRequest)
		return
	}

	info := readFwDeviceInfo(mac)
	dev := strings.ToLower(info.Dev)
	if dev == "" {
		dev = "hdd"
	}

	cmd, _ := json.Marshal(map[string]string{"module": "all", "action": "fw_update_abort", "dev": dev, "mac": mac})
	if err := sendToCmdSocket(string(cmd)); err != nil {
		logErrorf("handlePostFirmwareAbort - failed to send cmd socket: %v", err)
		http.Error(w, "cannot reach the core (ce_cores)", http.StatusBadGateway)
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

// writeFileAtomically - temp file + rename, so ce_cores never reads a half written file
func writeFileAtomically(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}

	tmp, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*")
	if err != nil {
		return err
	}

	_, werr := tmp.Write(data)
	cerr := tmp.Close()
	if werr != nil || cerr != nil {
		os.Remove(tmp.Name())
		if werr != nil {
			return werr
		}
		return cerr
	}

	if err := os.Rename(tmp.Name(), path); err != nil {
		os.Remove(tmp.Name())
		return err
	}
	return nil
}
