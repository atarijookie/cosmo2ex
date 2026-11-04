// ce_media - the host side of CE_VIDEO.TTP / CE_AUDIO.TTP: decodes media files with ffmpeg and converts them to
// what the Atari shows and plays (screen memory + palette, 8 bit DMA sound or YM volume table indexes), packed in
// chunks of one ACSI transfer each. ce_cores starts it (MEDIA_BIN in .env) and hands the chunks to the ST.
//
//	ce_media serve -connect 127.0.0.1:<port> [decoder options]      started by ce_cores
//	ce_media preview [options] <file>                               what the ST would get, as PNG + WAV files
//	ce_media probe [decoder options] <file>                         what ffprobe says about the file
//
// decoder options:
//
//	-ffmpeg <path>          ffmpeg binary (default: ffmpeg in PATH)
//	-ffprobe <path>         ffprobe binary (default: ffprobe in PATH, or next to -ffmpeg)
//	-docker-image <image>   run ffmpeg / ffprobe in this docker image instead (e.g. linuxserver/ffmpeg)
//	-docker <path>          docker binary (default: docker)
//	-ym-table <file>        plain ST sound: YM volume table from this file (256 x 16 B, as ST demos used)
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"
)

func decoderFlags(fs *flag.FlagSet) *Decoder {
	d := &Decoder{}
	d.imageInit()
	fs.StringVar(&d.ffmpeg, "ffmpeg", "ffmpeg", "ffmpeg binary")
	fs.StringVar(&d.ffprobe, "ffprobe", "", "ffprobe binary (default: next to -ffmpeg)")
	fs.StringVar(&d.dockerImage, "docker-image", "", "run ffmpeg / ffprobe in this docker image")
	fs.StringVar(&d.docker, "docker", "docker", "docker binary")
	fs.StringVar(&d.ymTablePath, "ym-table", "", "plain ST sound: YM volume table file (256 x 16 B) instead of the built one")
	return d
}

func (d *Decoder) fixup() {
	if d.ffprobe == "" {
		d.ffprobe = "ffprobe"
		if dir := filepath.Dir(d.ffmpeg); dir != "." || strings.ContainsAny(d.ffmpeg, `/\`) {
			base := strings.Replace(filepath.Base(d.ffmpeg), "ffmpeg", "ffprobe", 1)
			d.ffprobe = filepath.Join(dir, base)
		}
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `usage:
  ce_media serve -connect 127.0.0.1:<port> [decoder options]
  ce_media preview [-machine st|ste|tt|falcon] [-vmode st-low|st-high|tt-mid|tt-high|none] [-audio dma|ym|none]
                   [-start <s>] [-seconds <s>] [-frames <n>] [-out <dir>] [decoder options] <file>
  ce_media probe [decoder options] <file>
decoder options: -ffmpeg <path> -ffprobe <path> -docker-image <image> -docker <path> -ym-table <file>`)
	os.Exit(2)
}

func main() {
	log.SetFlags(log.Ltime | log.Lmicroseconds)
	log.SetPrefix("ce_media: ")
	if len(os.Args) < 2 {
		usage()
	}

	switch os.Args[1] {
	case "serve":
		fs := flag.NewFlagSet("serve", flag.ExitOnError)
		addr := fs.String("connect", "", "address of ce_cores (127.0.0.1:port)")
		dec := decoderFlags(fs)
		fs.Parse(os.Args[2:])
		dec.fixup()
		if *addr == "" {
			usage()
		}
		if err := serve(*addr, dec); err != nil {
			log.Fatal(err)
		}

	case "preview":
		os.Exit(previewMain(os.Args[2:]))

	case "probe":
		fs := flag.NewFlagSet("probe", flag.ExitOnError)
		dec := decoderFlags(fs)
		fs.Parse(os.Args[2:])
		dec.fixup()
		if fs.NArg() != 1 {
			usage()
		}
		if err := dec.waitImage(nil, func(m string) { fmt.Println(m) }); err != nil {
			log.Fatal(err)
		}
		p, err := dec.probe(fs.Arg(0))
		dec.stopWorker()
		dec.waitCleanups(5 * time.Second)
		if err != nil {
			log.Fatal(err)
		}
		fmt.Printf("title:    %s\nduration: %d ms\n", p.title, p.durationMs)
		if p.hasVideo {
			fmt.Printf("video:    stream %d, %dx%d, aspect %.3f, %d/%d fps\n", p.videoIndex, p.width, p.height, p.dar, p.fpsNum, p.fpsDen)
		}
		if p.hasAudio {
			fmt.Printf("audio:    stream %d\n", p.audioIndex)
		}

	default:
		usage()
	}
}
