#ifndef __P_UDMF_H
#define __P_UDMF_H

#include "sc_man.h"
#include "m_fixed.h"
#include <cstring>

class UDMFParserBase
{
protected:
	FScanner sc;
	FName namespc = NAME_None;
	int namespace_bits;
	FString parsedString;
	bool BadCoordinates = false;

	void Skip();
	FName ResolveKey(const char *text);
	FName ParseKey(bool checkblock = false, bool *isblock = NULL);
	int CheckInt(FName key);
	double CheckFloat(FName key);
	double CheckCoordinate(FName key);
	DAngle CheckAngle(FName key);
	bool CheckBool(FName key);
	const char *CheckString(FName key);
	int MatchString(FName key, const char* const* strings, int defval);

	template<typename T>
	bool Flag(T &value, int mask, FName key)
	{
		if (CheckBool(key))
		{
			value |= mask;
			return true;
		}
		else
		{
			value &= ~mask;
			return false;
		}
	}

private:
	struct KeyHashTraits
	{
		hash_t Hash(const char *text) { return (hash_t)SuperFastHash(text, strlen(text)); }
		int Compare(const char *left, const char *right) { return strcmp(left, right); }
	};
	// Copy scanner strings before caching. Character storage survives TArray
	// growth; destroy the lookup map before its owning names. Exact spellings
	// are cached; FName remains the authority for case-insensitive identity.
	TArray<FString> KeyNames;
	TMap<const char *, FName, KeyHashTraits> KeyCache;
};

#define BLOCK_ID (ENamedName)-1

#endif
