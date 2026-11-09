// ce_floppy_gen generates floppy test images with chosen geometry and known content, to test the floppy path on an
// Atari ST: .st, .msa and .stx (plain, without any protection), 8 to 11 sectors per track, 1 or 2 sides.
//
// Sector data is either TrSiSe - the bytes test_ce/test_floppy expects from its test image: byte N of the sector has
// value N, except of bytes 0, 1, 2, which hold the track, side and sector - or random, which is reproducible with the
// same seed. So the images have no file system: TOS can read their sectors, but the desktop can't open them.
package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const sectorSize = 512

// Geometry and content of one image.
type Spec struct {
	Tracks int
	Sides  int
	Spt    int
	Data   string // "trsise" or "random"
	Seed   uint32
}

func (s Spec) validate() error {
	if s.Tracks < 1 || s.Tracks > 85 {
		return fmt.Errorf("tracks must be 1 .. 85, got %d", s.Tracks)
	}
	if s.Sides != 1 && s.Sides != 2 {
		return fmt.Errorf("sides must be 1 or 2, got %d", s.Sides)
	}
	if s.Spt < 8 || s.Spt > 11 {
		return fmt.Errorf("sectors per track must be 8 .. 11, got %d", s.Spt)
	}
	if s.Data != "trsise" && s.Data != "random" {
		return fmt.Errorf("data must be trsise or random, got %q", s.Data)
	}
	return nil
}

// sectorData returns the content of a sector, sector numbers start at 1.
func (s Spec) sectorData(track, side, sector int) []byte {
	data := make([]byte, sectorSize)

	if s.Data == "trsise" {
		for i := range data {
			data[i] = byte(i)
		}
		data[0] = byte(track)
		data[1] = byte(side)
		data[2] = byte(sector)
		return data
	}

	// xorshift32 - own generator and own state for each sector, so the content never depends on the Go version or on the
	// image format, and an image with other geometry has the same data in the same sector
	x := s.Seed*2654435761 ^ uint32(track)<<16 ^ uint32(side)<<24 ^ uint32(sector)<<8 ^ 0x9e3779b9
	if x == 0 {
		x = 1
	}
	for i := range data {
		x ^= x << 13
		x ^= x >> 17
		x ^= x << 5
		data[i] = byte(x >> 11)
	}
	return data
}

func (s Spec) trackData(track, side int) []byte {
	data := make([]byte, 0, s.Spt*sectorSize)
	for sector := 1; sector <= s.Spt; sector++ {
		data = append(data, s.sectorData(track, side, sector)...)
	}
	return data
}

// Tracks in the order all the formats use: track 0 side 0, track 0 side 1, track 1 side 0...
func (s Spec) forEachTrack(f func(track, side int)) {
	for track := 0; track < s.Tracks; track++ {
		for side := 0; side < s.Sides; side++ {
			f(track, side)
		}
	}
}

// fileName makes the name of an image, with everything about it in the name.
func (s Spec) fileName(format string) string {
	return fmt.Sprintf("%dt_%dspt_%ds_%s.%s", s.Tracks, s.Spt, s.Sides, s.Data, format)
}

func generate(format string, s Spec) ([]byte, error) {
	if err := s.validate(); err != nil {
		return nil, err
	}

	switch format {
	case "st":
		return generateST(s), nil
	case "msa":
		return generateMSA(s), nil
	case "stx":
		return generateSTX(s), nil
	}
	return nil, fmt.Errorf("format must be st, msa or stx, got %q", format)
}

func writeImage(path, format string, s Spec) error {
	image, err := generate(format, s)
	if err != nil {
		return err
	}
	if err := os.WriteFile(path, image, 0o644); err != nil {
		return err
	}
	fmt.Printf("%s: %d tracks, %d side(s), %d sectors per track, %s data, %d bytes\n", path, s.Tracks, s.Sides, s.Spt, s.Data, len(image))
	return nil
}

// writeAll generates all the geometries the floppy path has to handle in all formats.
func writeAll(dir string, tracks int, seed uint32) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	for _, format := range []string{"st", "msa", "stx"} {
		for _, spt := range []int{8, 9, 10} {
			for _, sides := range []int{1, 2} {
				for _, data := range []string{"trsise", "random"} {
					s := Spec{Tracks: tracks, Sides: sides, Spt: spt, Data: data, Seed: seed}
					if err := writeImage(filepath.Join(dir, s.fileName(format)), format, s); err != nil {
						return err
					}
				}
			}
		}
	}
	return nil
}

func run(args []string) error {
	fs := flag.NewFlagSet("ce_floppy_gen", flag.ContinueOnError)
	format := fs.String("format", "st", "image format: st, msa, stx")
	spt := fs.Int("spt", 9, "sectors per track: 8 .. 11")
	sides := fs.Int("sides", 2, "sides: 1 or 2")
	data := fs.String("data", "trsise", "sector data: trsise (for test_floppy) or random")
	tracks := fs.Int("tracks", 80, "tracks: 1 .. 85")
	seed := fs.Uint("seed", 1, "seed of the random data")
	out := fs.String("o", "", "output file, default: name made from the parameters")
	all := fs.String("all", "", "generate all formats, 8/9/10 sectors per track, 1/2 sides, both data types to this directory")
	fs.Usage = func() {
		fmt.Fprintf(fs.Output(), "usage: ce_floppy_gen [options]\n   or: ce_floppy_gen -all DIR [-tracks N] [-seed N]\n\n")
		fs.PrintDefaults()
	}

	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() > 0 {
		return fmt.Errorf("unexpected argument: %s", strings.Join(fs.Args(), " "))
	}

	if *all != "" {
		return writeAll(*all, *tracks, uint32(*seed))
	}

	s := Spec{Tracks: *tracks, Sides: *sides, Spt: *spt, Data: *data, Seed: uint32(*seed)}
	path := *out
	if path == "" {
		path = s.fileName(*format)
	}
	return writeImage(path, *format, s)
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		if !errors.Is(err, flag.ErrHelp) {
			fmt.Fprintln(os.Stderr, "error:", err)
		}
		os.Exit(2)
	}
}
