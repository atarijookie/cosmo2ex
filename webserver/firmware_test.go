package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
)

const fwTestMac = "02CE00112233"

// fake firmware image: layout block (FS 0x180000..0x1ff000) and identity like a real one
func fwTestImage(t *testing.T, size int, ident string, fsStart, fsEnd uint32) []byte {
	t.Helper()
	bin := make([]byte, size)
	for i := range bin {
		bin[i] = byte(i * 13)
	}
	binary.LittleEndian.PutUint32(bin[fwLayoutOff:], fwFlashBase+fsStart)
	binary.LittleEndian.PutUint32(bin[fwLayoutOff+4:], fwFlashBase+fsEnd)
	if ident != "" {
		copy(bin[0x3100:], append([]byte(fwIdentPrefix+ident), 0))
	}
	return bin
}

// UF2 as picotool makes it: 256 B per block; RP2350 also gets the 'absolute' block first
func fwTestUF2(bin []byte, family uint32) []byte {
	var blocks [][]byte
	le := binary.LittleEndian

	mk := func(addr uint32, data []byte, fam uint32) []byte {
		b := make([]byte, 512)
		le.PutUint32(b[0:], uf2Magic0)
		le.PutUint32(b[4:], uf2Magic1)
		le.PutUint32(b[8:], uf2FlagFamilyID)
		le.PutUint32(b[12:], addr)
		le.PutUint32(b[16:], 256)
		le.PutUint32(b[28:], fam)
		copy(b[32:], data)
		le.PutUint32(b[508:], uf2MagicEnd)
		return b
	}

	if family == uf2FamilyRP2350ARM {
		blocks = append(blocks, mk(0x10ffff00, make([]byte, 256), uf2FamilyAbsolute))
	}
	for off := 0; off < len(bin); off += 256 {
		end := off + 256
		if end > len(bin) {
			end = len(bin)
		}
		blocks = append(blocks, mk(uint32(fwFlashBase+off), bin[off:end], family))
	}
	for i, b := range blocks {
		le.PutUint32(b[20:], uint32(i))
		le.PutUint32(b[24:], uint32(len(blocks)))
	}
	return bytes.Join(blocks, nil)
}

// device that reported its firmware - like ChipInterface::storeFwInfo() writes it
func fwTestDevice(t *testing.T, connected bool, running []byte) string {
	t.Helper()
	settings := t.TempDir()
	t.Setenv("SETTINGS_DIR", settings)
	t.Setenv("CMD_UDP_PORT", "")

	devFile := filepath.Join(t.TempDir(), "connected")
	content := ""
	if connected {
		content = fwTestMac + "\n"
	}
	if err := os.WriteFile(devFile, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("CONNECTED_DEVICES_FILE", devFile)

	dir := filepath.Join(settings, fwTestMac)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}

	size := len(running) - 100 // the device hashes without the UF2 padding
	for k, v := range map[string]string{
		"FW_IDENT":      "dev=HDD;type=W6100-EVB-PICO;chip=RP2040;build=v1.0.0;",
		"FW_VERSION":    "2026-11-14",
		"FW_IMAGE_SIZE": strconv.Itoa(size),
		"FW_IMAGE_MD5":  strings.ToUpper(md5Hex(running[:size])),
		"FW_FS_START":   strconv.Itoa(0x180000),
		"FW_FS_END":     strconv.Itoa(0x1ff000),
		"FW_OTA":        "1",
	} {
		if err := os.WriteFile(filepath.Join(dir, k), []byte(v), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

const fwTestIdent = "dev=HDD;type=W6100-EVB-PICO;chip=RP2040;build=v1.0.1;"

func TestParseUF2(t *testing.T) {
	bin := fwTestImage(t, 0x4000+100, fwTestIdent, 0x180000, 0x1ff000)

	img, err := parseUF2(fwTestUF2(bin, uf2FamilyRP2040))
	if err != nil {
		t.Fatal(err)
	}
	if img.Chip != "RP2040" || len(img.Data) != 0x4000+256 || !bytes.Equal(img.Data[:len(bin)], bin) {
		t.Errorf("RP2040: chip %s, %d B", img.Chip, len(img.Data))
	}

	img, err = parseUF2(fwTestUF2(bin, uf2FamilyRP2350ARM)) // the absolute block is skipped
	if err != nil || img.Chip != "RP2350" || !bytes.Equal(img.Data[:len(bin)], bin) {
		t.Errorf("RP2350: %v %+v", err, img)
	}

	if _, err := parseUF2([]byte("not an uf2 file")); err == nil {
		t.Error("garbage accepted")
	}

	uf2 := fwTestUF2(bin, uf2FamilyRP2040)
	gap := append(append([]byte{}, uf2[:512*3]...), uf2[512*4:]...) // one block missing
	if _, err := parseUF2(gap); err == nil || !strings.Contains(err.Error(), "contiguous") {
		t.Errorf("gap: %v", err)
	}

	other := fwTestUF2(bin, 0xada52840) // nRF52840
	if _, err := parseUF2(other); err == nil {
		t.Error("other family accepted")
	}

	if id := fwIdentOf(bin); id["dev"] != "HDD" || id["type"] != "W6100-EVB-PICO" || id["build"] != "v1.0.1" {
		t.Errorf("ident: %v", id)
	}
	if fwFileName("HDD", "W6100-EVB-PICO") != "ce_pico_hdd_W6100-EVB-PICO.uf2" {
		t.Error(fwFileName("HDD", "W6100-EVB-PICO"))
	}
}

func TestCheckFirmware(t *testing.T) {
	running := fwTestImage(t, 0x8000, "dev=HDD;type=W6100-EVB-PICO;chip=RP2040;build=v1.0.0;", 0x180000, 0x1ff000)
	running[0x5000] = 0xaa
	fwTestDevice(t, true, running)
	info := readFwDeviceInfo(fwTestMac)

	if !info.Reported || info.NeededFile != "ce_pico_hdd_W6100-EVB-PICO.uf2" || !info.Connected || !info.OTA {
		t.Fatalf("info: %+v", info)
	}

	newFw := fwTestImage(t, 0x8000, fwTestIdent, 0x180000, 0x1ff000)

	cases := []struct {
		name    string
		info    func(i fwDeviceInfo) fwDeviceInfo
		bin     []byte
		chip    string
		problem string // "" = must be OK
	}{
		{"ok", nil, newFw, "RP2040", ""},
		{"same firmware", nil, running, "RP2040", "already running"},
		{"other chip", nil, newFw, "RP2350", "for the RP2350 chip"},
		{"other device", nil, fwTestImage(t, 0x8000, "dev=FDD;type=PICO-2W;chip=RP2350;build=x;", 0x180000, 0x1ff000), "", "is for FDD PICO-2W"},
		{"other board", nil, fwTestImage(t, 0x8000, "dev=HDD;type=W6100-EVB-PICO2;chip=RP2350;build=x;", 0x180000, 0x1ff000), "", "you need ce_pico_hdd_W6100-EVB-PICO.uf2"},
		{"no identity", nil, fwTestImage(t, 0x8000, "", 0x180000, 0x1ff000), "", "not a CosmosEx firmware"},
		{"other layout", nil, fwTestImage(t, 0x8000, fwTestIdent, 0x100000, 0x1ff000), "", "different flash layout"},
		{"no fs", nil, fwTestImage(t, 0x8000, fwTestIdent, 0x1ff000, 0x1ff000), "", "different flash layout"},
		{"too big", nil, fwTestImage(t, 0x180000+10, fwTestIdent, 0x180000, 0x1ff000), "", "too big"},
		{"not connected", func(i fwDeviceInfo) fwDeviceInfo { i.Connected = false; return i }, newFw, "", "not connected"},
		{"old firmware", func(i fwDeviceInfo) fwDeviceInfo { i.Reported = false; return i }, newFw, "", "via USB"},
		{"built without fs", func(i fwDeviceInfo) fwDeviceInfo { i.OTA = false; return i }, newFw, "", "without the file system"},
	}

	for _, c := range cases {
		i := info
		if c.info != nil {
			i = c.info(i)
		}
		problems := checkFirmware(i, c.bin, c.chip)
		all := strings.Join(problems, " | ")

		if c.problem == "" && len(problems) > 0 {
			t.Errorf("%s: unexpected problems: %s", c.name, all)
		}
		if c.problem != "" && !strings.Contains(all, c.problem) {
			t.Errorf("%s: want %q, got %q", c.name, c.problem, all)
		}
	}
}

func TestFirmwareHandlers(t *testing.T) {
	running := fwTestImage(t, 0x8000, "dev=HDD;type=W6100-EVB-PICO;chip=RP2040;build=v1.0.0;", 0x180000, 0x1ff000)
	dir := fwTestDevice(t, true, running)
	s := &Server{}

	call := func(handler http.HandlerFunc, method, path string, body []byte) *httptest.ResponseRecorder {
		t.Helper()
		rc := chi.NewRouteContext()
		rc.URLParams.Add("mac", "02:ce:00:11:22:33")
		r := httptest.NewRequest(method, path, bytes.NewReader(body))
		r = r.WithContext(context.WithValue(r.Context(), chi.RouteCtxKey, rc))
		w := httptest.NewRecorder()
		handler(w, r)
		return w
	}

	check := func(uf2 []byte) fwCheckResult {
		t.Helper()
		w := call(s.handlePostFirmwareCheck, http.MethodPost, "/device/x/firmware/check", uf2)
		if w.Code != http.StatusOK {
			t.Fatalf("check: %d %s", w.Code, w.Body.String())
		}
		var res fwCheckResult
		if err := json.Unmarshal(w.Body.Bytes(), &res); err != nil {
			t.Fatal(err)
		}
		return res
	}

	// wrong file - refused, nothing stored
	res := check(fwTestUF2(fwTestImage(t, 0x8000, "dev=FDD;type=PICO-2W;chip=RP2350;build=x;", 0x180000, 0x1ff000), uf2FamilyRP2350ARM))
	if res.OK || len(res.Problems) == 0 {
		t.Errorf("wrong file accepted: %+v", res)
	}
	if _, err := os.Stat(filepath.Join(dir, fwUpdateBinKey)); err == nil {
		t.Error("refused image was stored")
	}

	// good file - stored for the update
	newFw := fwTestImage(t, 0x8000+50, fwTestIdent, 0x180000, 0x1ff000)
	res = check(fwTestUF2(newFw, uf2FamilyRP2040))
	if !res.OK || res.File == nil || res.File.Build != "v1.0.1" || res.File.Chip != "RP2040" {
		t.Fatalf("good file: %+v", res)
	}
	stored, err := os.ReadFile(filepath.Join(dir, fwUpdateBinKey))
	if err != nil || !bytes.Equal(stored[:len(newFw)], newFw) {
		t.Fatalf("stored: %v", err)
	}

	// update - no core to send it to here, but the status says it was requested
	w := call(s.handlePostFirmwareUpdate, http.MethodPost, "/device/x/firmware/update", nil)
	if w.Code != http.StatusBadGateway {
		t.Errorf("update without core: %d", w.Code)
	}

	w = call(s.handleGetFirmware, http.MethodGet, "/device/x/firmware", nil)
	var info fwDeviceInfo
	if err := json.Unmarshal(w.Body.Bytes(), &info); err != nil {
		t.Fatal(err)
	}
	if info.PendingFile == nil || info.PendingFile.Build != "v1.0.1" || info.PendingRuns || !strings.Contains(string(info.Update), `"requested"`) {
		t.Errorf("get: %+v, update %s", info, info.Update)
	}

	// requested a moment ago - another update is refused
	if res := check(fwTestUF2(newFw, uf2FamilyRP2040)); res.OK {
		t.Error("second update accepted while the first one runs")
	}

	// the device rebooted into the new firmware and reported it - the update is done
	size := len(newFw)
	os.WriteFile(filepath.Join(dir, "FW_IMAGE_SIZE"), []byte(strconv.Itoa(size)), 0o644)
	os.WriteFile(filepath.Join(dir, "FW_IMAGE_MD5"), []byte(md5Hex(newFw)), 0o644)
	os.WriteFile(filepath.Join(dir, "FW_STATE"), []byte("trial"), 0o644)
	if info := readFwDeviceInfo(fwTestMac); !info.PendingRuns || info.State != "trial" {
		t.Errorf("new firmware runs, but not detected: %v %s", info.PendingRuns, info.State)
	}
}
