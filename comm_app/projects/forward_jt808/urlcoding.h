#ifndef _URLCODING_H_
#define _URLCODING_H_

#include <string>
using std::string;
#include <vector>
using std::vector;

class UrlCoding {
	static const char _EnTab[0x11];
	static const char _DeTab[0xff];

	const char _tag;
public:
	UrlCoding(char tag);

	string encode(const string &arg);
	string decode(const string &arg);
};

#endif//_URLCODING_H_
