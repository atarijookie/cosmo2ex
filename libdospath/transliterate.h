#ifndef TRANSLITERATE_H
#define TRANSLITERATE_H

#include <string>

// Replace UTF-8 chars with diacritics (and Greek / Cyrillic letters) with ASCII equivalents, e.g. 'Červený' -> 'Cerveny'.
// Valid UTF-8 chars without equivalent become single '_', bytes which are not valid UTF-8 are copied unchanged.
void transliterateToAscii(const std::string& in, std::string& out);

#endif // TRANSLITERATE_H
