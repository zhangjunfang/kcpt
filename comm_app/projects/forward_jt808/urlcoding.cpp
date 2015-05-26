#include "urlcoding.h"

const char UrlCoding::_EnTab[] = "0123456789abcdef";
const char UrlCoding::_DeTab[] = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x0\x1\x2\x3\x4\x5\x6\x7\x8\x9\0\0\0\0\0\0\0"
	"\xa\xb\xc\xd\xe\xf\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\xa\xb\xc\xd\xe\xf";


UrlCoding::UrlCoding(char tag) : _tag(tag) {
}

string UrlCoding::encode(const string &arg) {
	unsigned char ch;
	string::const_iterator it;

	vector<char> buf(arg.length() * 3 + 1);
	char *ptr = &buf[0];
	size_t len = 0;

	for (it = arg.begin(); it != arg.end(); ++it) {
		ch = *it;
		switch (ch) {
		case '0' ... '9':
		case 'a' ... 'z':
		case 'A' ... 'Z':
			ptr[len++] = ch;
			break;
		default:
			ptr[len++] = _tag;
			ptr[len++] = _EnTab[ch >> 4];
			ptr[len++] = _EnTab[ch & 0xf];
			break;
		}
	}

	return string(ptr, len);
}

string UrlCoding::decode(const string &arg) {
	unsigned char ch;
	string::const_iterator it;

	vector<char> buf(arg.length() + 1);
	char *ptr = &buf[0];
	size_t len = 0;

	unsigned char val = 0;
	unsigned char num = 0;
	for (it = arg.begin(); it != arg.end(); ++it) {
		ch = *it;
		if (ch == _tag) {
			val = 0;
			num = 1;
		} else if (num == 0) {
			val = 0;
			ptr[len++] = ch;
		} else if (num == 1) {
			val = _DeTab[ch] << 4;
			num = 2;
		} else if (num == 2) {
			val = val | _DeTab[ch];
			num = 0;
			ptr[len++] = val;
		}
	}

	return string(ptr, len);
}


