#include "../root_mytypedef.h"
#include "purelib.h"

#define __HI(x) *(1 + (int *)&x)
#define __LO(x) *(int *)&x
#define __HIp(x) *(1 + (int *)x)
#define __LOp(x) *(int *)x

#define MAXFLOAT ((float)3.40282346638528860e+38)

//extern _LIB_VERSION_TYPE  _LIB_VERSION;

#define _IEEE_ fdlibm_ieee
#define _SVID_ fdlibm_svid
#define _XOPEN_ fdlibm_xopen
#define _POSIX_ fdlibm_posix

//#define	HUGE		MAXFLOAT

/*
 * set X_TLOSS = pi*2**52, which is possibly defined in <values.h>
 * (one may replace the following line by "#include <values.h>")
 */

#define X_TLOSS 1.41484755040568800000e+16
#define DOMAIN 1
#define SING 2
#define OVERFLOW 3
#define UNDERFLOW 4
#define TLOSS 5
#define PLOSS 6

#define SEEK_CUR 1
#define SEEK_END 2
#define SEEK_SET 0

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 0xff

#define MB_LEN_MAX 5
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 0xffff
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 0xffffffff
#define LONG_MIN (-2147483647L - 1)
#define LONG_MAX 2147483647L
#define ULONG_MAX 0xffffffffUL
#define LLONG_MAX 9223372036854775807i64
#define LLONG_MIN (-9223372036854775807i64 - 1)
#define ULLONG_MAX 0xffffffffffffffffui64

static int _easyclib_internal__is_character_space(unsigned char c)
{
	return (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == ' ' ? 1 : 0);
}

long purelib__strtol(const char *str, char **endptr, int base)
{
	const char *s;
	long acc;
	char c;
	unsigned long cutoff;
	int neg, any, cutlim;

	/*
     * Skip white space and pick up leading +/- sign if any.
     * If base is 0, allow 0x for hex and 0 for octal, else
     * assume decimal; if base is already 16, allow 0x.
     */
	s = str;
	do
	{
		c = *s++;
	} while (_easyclib_internal__is_character_space((unsigned char)c));

	if (c == '-')
	{
		neg = 1;
		c = *s++;
	}
	else
	{
		neg = 0;
		if (c == '+')
		{
			c = *s++;
		}
	}
	if ((base == 0 || base == 16) && c == '0' && (*s == 'x' || *s == 'X') && ((s[1] >= '0' && s[1] <= '9') || (s[1] >= 'A' && s[1] <= 'F') || (s[1] >= 'a' && s[1] <= 'f')))
	{
		c = s[1];
		s += 2;
		base = 16;
	}
	if (base == 0)
	{
		base = c == '0' ? 8 : 10;
	}
	acc = any = 0;
	if (base < 2 || base > 36)
	{
		return 1;
		//zmazane goto: noconv
	}

	/*
     * Compute the cutoff value between legal numbers and illegal
     * numbers.  That is the largest legal value, divided by the
     * base.  An input number that is greater than this value, if
     * followed by a legal input character, is too big.  One that
     * is equal to this value may be valid or not; the limit
     * between valid and invalid numbers is then based on the last
     * digit.  For instance, if the range for longs is
     * [-2147483648..2147483647] and the input base is 10,
     * cutoff will be set to 214748364 and cutlim to either
     * 7 (neg==0) or 8 (neg==1), meaning that if we have accumulated
     * a value > 214748364, or equal but the next digit is > 7 (or 8),
     * the number is too big, and we will return a range error.
     *
     * Set 'any' if any `digits' consumed; make it negative to indicate
     * overflow.
     */
	cutoff = neg ? (unsigned long)-(LONG_MIN + LONG_MAX) + LONG_MAX : LONG_MAX;
	cutlim = cutoff % base;
	cutoff /= base;

	for (;; c = *s++)
	{
		if (c >= '0' && c <= '9')
		{
			c -= '0';
		}
		else if (c >= 'A' && c <= 'Z')
		{
			c -= 'A' - 10;
		}
		else if (c >= 'a' && c <= 'z')
		{
			c -= 'a' - 10;
		}
		else
		{
			break;
		}
		if (c >= base)
		{
			break;
		}
		if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
		{
			any = -1;
		}
		else
		{
			any = 1;
			acc *= base;
			acc += c;
		}
	}
	if (any < 0)
	{
		acc = neg ? LONG_MIN : LONG_MAX;
	}
	else if (!any)
	{
		//nocov label
	}
	else if (neg)
	{
		acc = -acc;
	}
	if (endptr != NULL)
	{
		*endptr = (char *)(any ? s - 1 : str);
	}
	return (acc);
}

size_t purelib__wcslen(const wchar_t *str)
{
	//ASCII string
	size_t curr_string_length = 0;

	//string length limit, 1000

	size_t i;
	for (i = 0; i < 1000; i++)
	{
		wchar_t single_char = *(str + i);
		if (single_char == 0)
		{
			break;
		}
		curr_string_length++;
	}
	return curr_string_length;
}

size_t purelib__strlen(const char *str)
{
	//ASCII string
	size_t curr_string_length = 0;

	//string length limit, 1000

	size_t i;
	for (i = 0; i < 1000; i++)
	{
		char single_char = *(str + i);
		if (single_char == 0)
		{
			break;
		}
		curr_string_length++;
	}
	return curr_string_length;
}

int purelib__strcmp(const char *str1, const char *str2)
{
	while (*str1 && *str2) //pokial obidva stringy nenarazia na null terminator
	{
		if (*str1 != *str2)
		{
			break;
		}
		str1++;
		str2++;
	}
	return (int)*str1 - (int)*str2; //ak jeden string narazil na null terminator, mal by narazit aj druhy.
}

int purelib__stricmp(const char *str1, const char *str2)
{
	while (*str1 && *str2)
	{
		if ((*str1 | 0x20) != (*str2 | 0x20))
		{
			break;
		}
		str1++;
		str2++;
	}
	return (int)*str1 - (int)*str2;
}

int purelib__strncmp(const char *str1, const char *str2, size_t n)
{
	size_t current_byte = 0;
	while (current_byte < n) //pokial obidva stringy nenarazia na null terminator
	{
		if (*str1 != *str2)
		{
			break;
		}
		str1++;
		str2++;
		current_byte++;
	}
	return (int)*str1 - (int)*str2; //ak jeden string narazil na null terminator, mal by narazit aj druhy.
}

int purelib__wcscmp(const wchar_t *str1, const wchar_t *str2)
{
	int result = 0;

	if (str1 == NULL_POINTER || str2 == NULL_POINTER)
	{
		result = 1;
		return result;
	}

	size_t a = purelib__wcslen(str1);
	size_t b = purelib__wcslen(str2);

	//najprv sa skontroluje ci su rovnake dlzky stringu, dava to viac zmysel?
	//ani nie

	if (a != b)
	{
		result = 1;
	}
	else
	{
		int i;
		for (i = 0; i < a; i++)
		{
			char byte1 = str1[i];
			char byte2 = str2[i];
			if (byte1 != byte2)
			{
				result = 1;
				break;
			}
		}
	}
	return result;
}

void purelib__null_memory(void *source, size_t length)
{
	if (source == 0)
	{
		return;
	}
	char *src = source;

	while (length--)
	{
		*src++ = (char)0;
	}
}

void purelib__copy_memory(void *source, void *destination, size_t length, size_t max_allowed_length)
{
	size_t already_copied_bytes = 0;
	if (source == 0 || destination == 0)
	{
		return;
	}
	char *src = (char *)source;

	char *dest = (char *)destination;

	while (length--)
	{
		*dest++ = *src++;
		already_copied_bytes++;
		if (already_copied_bytes >= max_allowed_length)
		{
			break;
		}
	}
}

int purelib__min(int a, int b)
{
	return (a < b) ? a : b;
}

uint64 purelib__min_64(uint64 a, uint64 b)
{
	return (a < b) ? a : b;
}
