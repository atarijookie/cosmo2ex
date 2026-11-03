package main

import "testing"

func TestToAtariASCII(t *testing.T) {
	cases := map[string]string{
		"SME | Najčítanejšie správy zo Slovenska": "SME | Najcitanejsie spravy zo Slovenska",
		"Atari ST – Wikipedia":                    "Atari ST - Wikipedia",
		"Name · Line":                             "Name Line",
		"Straße • „Zitat“ … ©":                    "Strasse \"Zitat\" ... (c)",
		"  Ünïcödé\ttabs\nlines  ":                "Unicode tabs lines",
		"Łódź ÆØÅ":                                "Lodz AEOA",
		"emoji 😀 and 日本語 text":                    "emoji and text",
		"https://example.com/a?b=c":               "https://example.com/a?b=c",
		"":                                        "",
	}
	for in, want := range cases {
		if got := toAtariASCII(in); got != want {
			t.Errorf("toAtariASCII(%q) = %q, want %q", in, got, want)
		}
	}
}
