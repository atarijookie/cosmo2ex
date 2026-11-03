// ce_browser - host side of the Atari web browser: drives headless Chromium and converts pages to Atari video modes.
//
// Commands:
//
//	preview - renders pages in Atari modes to PNG files plus index.html, to see them on a PC
//	serve   - the browser for CE_BROWS.PRG, connected to ce_cores (BrowserModule, HOSTMOD_BROWSER) - serve.go
package main

import (
	"flag"
	"fmt"
	"html"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"time"
)

func usage() {
	fmt.Fprintf(os.Stderr, `usage: ce_browser preview [flags] URL...
       ce_browser serve -connect host:port [flags]   (started by ce_cores for CE_BROWS.PRG)

Renders each URL in Atari video modes and writes PNGs + index.html to the output directory.
Modes: `)
	for _, m := range modes {
		fmt.Fprintf(os.Stderr, "%s ", m.Name)
	}
	fmt.Fprintln(os.Stderr, "\n\nflags:")
	flag.PrintDefaults()
}

// chromeDefaultPath - where Chrome for Testing's chrome-headless-shell is expected when CHROME_PATH isn't set
func chromeDefaultPath() string {
	cache, err := os.UserCacheDir()
	if err != nil {
		return ""
	}
	plat := map[string]string{"linux/amd64": "linux64", "darwin/arm64": "mac-arm64", "darwin/amd64": "mac-x64",
		"windows/amd64": "win64"}[runtime.GOOS+"/"+runtime.GOARCH]
	exe := "chrome-headless-shell"
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	return filepath.Join(cache, "ce_browser", "chrome-headless-shell-"+plat, exe)
}

var slugRe = regexp.MustCompile(`[^a-zA-Z0-9]+`)

func slug(url string) string {
	s := strings.TrimPrefix(strings.TrimPrefix(url, "https://"), "http://")
	s = strings.Trim(slugRe.ReplaceAllString(s, "_"), "_")
	if len(s) > 60 {
		s = s[:60]
	}
	return s
}

func savePNG(path string, img image.Image) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return png.Encode(f, img)
}

type result struct {
	mode    *Mode
	file    string
	view    string
	note    string
	failErr error
}

func main() {
	if len(os.Args) >= 2 && os.Args[1] == "serve" {
		serveMain(os.Args[2:])
		return
	}
	if len(os.Args) < 2 || os.Args[1] != "preview" {
		flag.Usage = usage
		usage()
		os.Exit(2)
	}

	fs := flag.NewFlagSet("preview", flag.ExitOnError)
	fs.Usage = usage
	flag.CommandLine = fs
	outDir := fs.String("out", "preview_out", "output directory")
	modeList := fs.String("modes", "", "comma separated modes (default: all)")
	screens := fs.Int("screens", 3, "how many screen heights of each page to capture")
	chromePath := fs.String("chrome", os.Getenv("CHROME_PATH"), "path to chrome / chrome-headless-shell (default: $CHROME_PATH, then Chrome for Testing in user cache dir)")
	noAA := fs.Bool("noaa", true, "turn font anti-aliasing off through fontconfig (Linux only)")
	debug := fs.Bool("debug", false, "also save the layers and the text mask at Atari resolution")
	zoomList := fs.String("zoom", "1", "comma separated page zoom factors, each mode is rendered at each of them (e.g. 1,2)")
	fs.Parse(os.Args[2:])
	urls := fs.Args()
	if len(urls) == 0 {
		usage()
		os.Exit(2)
	}

	sel := modes
	if *modeList != "" {
		sel = nil
		for _, n := range strings.Split(*modeList, ",") {
			m := modeByName(strings.TrimSpace(n))
			if m == nil {
				fmt.Fprintf(os.Stderr, "unknown mode %q\n", n)
				os.Exit(2)
			}
			sel = append(sel, *m)
		}
	}

	var withZoom []Mode // each selected mode at each zoom, zooms of one mode next to each other
	for _, m := range sel {
		for _, zs := range strings.Split(*zoomList, ",") {
			z, err := strconv.ParseFloat(strings.TrimSpace(zs), 64)
			if err != nil || z < 0.25 || z > 4 {
				fmt.Fprintf(os.Stderr, "bad zoom %q (0.25 .. 4)\n", zs)
				os.Exit(2)
			}
			withZoom = append(withZoom, m.zoomed(z))
		}
	}
	sel = withZoom

	if *chromePath == "" {
		*chromePath = chromeDefaultPath()
	}
	if _, err := os.Stat(*chromePath); err != nil {
		fmt.Fprintf(os.Stderr, "chrome not found at %q - set CHROME_PATH or -chrome, or unpack Chrome for Testing's "+
			"chrome-headless-shell there (https://googlechromelabs.github.io/chrome-for-testing/)\n", *chromePath)
		os.Exit(1)
	}

	if err := os.MkdirAll(*outDir, 0755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	workDir, err := os.MkdirTemp("", "ce_browser")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer os.RemoveAll(workDir)

	b, err := startBrowser(*chromePath, *noAA, workDir)
	if err != nil {
		fmt.Fprintln(os.Stderr, "starting chrome:", err)
		os.Exit(1)
	}
	defer b.Close()

	logf := func(f string, a ...interface{}) { fmt.Printf(f+"\n", a...) }
	pages := map[string][]result{}

	for _, url := range urls {
		logf("%s", url)
		for i := range sel {
			m := &sel[i]
			logf("  %s", m.Name)
			r := result{mode: m}

			layers, err := b.capture(url, m, *screens, logf)
			if err != nil {
				logf("    FAILED: %v", err)
				r.failErr = err
				pages[url] = append(pages[url], r)
				continue
			}

			t0 := time.Now()
			h := layers.PageH * m.H / m.RenderH // Atari rows of the captured part
			text := resample(layers.Text, m.W, h)
			plain := resample(layers.Plain, m.W, h)
			imgs := resample(layers.Images, m.W, h)
			mask := textMask(text, plain)

			var out *image.Paletted
			if m.Planes == 1 {
				out = convertMono(text, imgs, mask)
			} else {
				out = convertColor(text, imgs, mask, m.Planes, m.ColorBits)
			}
			convMs := time.Since(t0).Milliseconds()

			base := slug(url) + "_" + m.Name
			if *debug { // layers at Atari resolution + text mask, to see why a pixel came out as it did
				savePNG(filepath.Join(*outDir, base+"_dbg_text.png"), text)
				savePNG(filepath.Join(*outDir, base+"_dbg_plain.png"), plain)
				savePNG(filepath.Join(*outDir, base+"_dbg_images.png"), imgs)
				mk := image.NewGray(text.Bounds())
				for i, t := range mask {
					if !t {
						mk.Pix[i] = 255
					}
				}
				savePNG(filepath.Join(*outDir, base+"_dbg_mask.png"), mk)
			}
			r.file = base + ".png"
			r.view = base + "_view.png"
			if err := savePNG(filepath.Join(*outDir, r.file), out); err != nil {
				logf("    save: %v", err)
			}
			if err := savePNG(filepath.Join(*outDir, r.view), scaleView(out, m.ViewSX, m.ViewSY)); err != nil {
				logf("    save: %v", err)
			}
			r.note = fmt.Sprintf("%d colours, %d x %d Atari px captured (%.1f screens), conversion %d ms",
				len(out.Palette), m.W, h, float64(h)/float64(m.H), convMs)
			logf("    %s", r.note)
			pages[url] = append(pages[url], r)
		}
	}

	if err := writeIndex(filepath.Join(*outDir, "index.html"), urls, pages); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	logf("done: %s", filepath.Join(*outDir, "index.html"))
}

// writeIndex - each mode shown in a box one Atari screen tall (at preview scale), scrollable through the captured part
func writeIndex(path string, urls []string, pages map[string][]result) error {
	var sb strings.Builder
	sb.WriteString(`<!doctype html><html><head><meta charset="utf-8"><title>ce_browser preview</title><style>
body{font:14px sans-serif;background:#555;color:#eee;margin:16px}
h2{margin:32px 0 8px} .modes{display:flex;flex-wrap:wrap;gap:24px;align-items:flex-start}
figure{margin:0} figcaption{margin:4px 0;max-width:1280px} small{color:#bbb}
.screen{overflow-y:auto;overflow-x:hidden;border:12px solid #222;border-radius:8px;background:#000}
.screen img{display:block;image-rendering:pixelated}
a{color:#9cf}
</style></head><body><h1>ce_browser preview</h1>
<p>Each box is one Atari screen, enlarged to the monitor's aspect ratio; scroll inside it to see the rest of the captured page.</p>
`)
	for _, url := range urls {
		fmt.Fprintf(&sb, "<h2>%s</h2><div class=\"modes\">\n", html.EscapeString(url))
		for _, r := range pages[url] {
			m := r.mode
			if r.failErr != nil {
				fmt.Fprintf(&sb, "<figure><figcaption><b>%s</b> failed: %s</figcaption></figure>\n",
					m.Name, html.EscapeString(r.failErr.Error()))
				continue
			}
			fmt.Fprintf(&sb, `<figure><figcaption><b>%s</b> - %s<br><small>%s - <a href="%s">Atari pixels</a></small></figcaption>`+
				`<div class="screen" style="width:%dpx;height:%dpx"><img src="%s"></div></figure>`+"\n",
				m.Name, html.EscapeString(m.Description), html.EscapeString(r.note), r.file,
				m.W*m.ViewSX, m.H*m.ViewSY, r.view)
		}
		sb.WriteString("</div>\n")
	}
	sb.WriteString("</body></html>\n")
	return os.WriteFile(path, []byte(sb.String()), 0644)
}
