#include "../root_mytypedef.h"
#include <Windows.h>
#include <Psapi.h>
#include <oleauto.h>
#include <winsock.h>

#include "windows/winapi.h"
#include "clib.h"
#include "memory_manager.h"
#include "console.h"

//this function was pasted from some gist, and is probably shit

//to fix
//double and float functions mix up, separate functions that deal with doubles and floats
//it does not crash, but its probably also not right way to do things

#define _CSTDLIB_

#define __HI(x) *(1 + (int *)&x)
#define __LO(x) *(int *)&x
#define __HIp(x) *(1 + (int *)x)
#define __LOp(x) *(int *)x

#define MAXFLOAT ((float)3.40282346638528860e+38)

//enum fdversion {fdlibm_ieee = -1, fdlibm_svid, fdlibm_xopen, fdlibm_posix};

//#define _LIB_VERSION_TYPE enum fdversion
//#define _LIB_VERSION _fdlib_version

/* if global variable _LIB_VERSION is not desirable, one may
 * change the following to be a constant by:
 *	#define _LIB_VERSION_TYPE const enum version
 * In that case, after one initializes the value _LIB_VERSION (see
 * s_lib_version.c) during compile time, it cannot be modified
 * in the middle of a program
 */

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

char **clib__split_str(char *string_to_split, int string_to_split_length, char *substring_used_in_splitting, int substring_used_in_splitting_length, int *splitted_substrings_count)
{
	char **result_array = 0;
	//find out positions and count of delimeter characters in string
	//create array of pointers to subtrings
	//in function arguments, change the value of "array_of_substrings" pointer so it will point to pointer that holds all found substrings

	//one - dymensional array where substring positions inside string will be stored
	//dynamically allocated to appropiate size

	//if found substring will be 3 chars long and located at the begging on base string
	//its positions will be 0,2
	//because its allocated with malloc, array must be freed at end
	int *substring_positions = (int *)memory_manager__alloc(sizeof(int) * (nuint)string_to_split_length * 2);

	//local variables, will be stored on stack
	int found_delimeters_count = 0;

	int found_substring_start_position = 0;

	int x;
	for (x = 0; x < string_to_split_length; x++)
	{
		//compare each char in delimeter with current position in base string
		//if chars do not match, skip check for current base position and continue with next one
		int i;
		for (i = 0; i < substring_used_in_splitting_length; i++)
		{
			//if char in string does not match, break loop
			if (string_to_split[x + i] != substring_used_in_splitting[i])
			{
				break;
			}

			//if char matches but it matches with a ending character that cant be used to split any string (there are no chars behind it) break
			if ((string_to_split[x + i] == substring_used_in_splitting[i]) && (string_to_split_length == (x + substring_used_in_splitting_length)))
			{
				DBG_CLIB console__write("%s %d %s", "DBG_CLIB cant split char at ", x, "\n");
				break;
			}

			//check if all chars in searched pattern were found.
			//if condition is met, it means substring
			if (i + 1 == substring_used_in_splitting_length)
			{
				//if searched string is found at beginning, what will change is found_substring start position
				if (x == 0)
				{
					found_substring_start_position = substring_used_in_splitting_length;
				}
				else
				{
					//store beginning and end positions of found substring in dynamically allocated array

					substring_positions[found_delimeters_count * 2] = found_substring_start_position;
					substring_positions[found_delimeters_count * 2 + 1] = x;
					//set position for next substring
					found_substring_start_position = x + substring_used_in_splitting_length;
				}

				DBG_CLIB console__write("%s %d %s", "DBG_CLIB equal at ", x, "\n");

				found_delimeters_count++;
			}
		}
	}

	DBG_CLIB console__write("%s %d %s", "DBG_CLIB clib__split_str found delimeters count ", found_delimeters_count, "\n");

	if (found_delimeters_count == 0)
	{
		memory_manager__free(substring_positions, TRUE);
		return 0;
	}

	//add one last found position of substring to strings
	//one that could not be set in loop
	substring_positions[found_delimeters_count * 2] = found_substring_start_position;
	substring_positions[found_delimeters_count * 2 + 1] = string_to_split_length;

	//find what should be the length of final array where pointers will be stored.
	//the same loop will be repeased later but for the purpose of extracting individual chars from base string
	//and putting them to substrings which will be then put to newly allocated array

	int actual_number_of_strings = 0;

	int y;
	//iteration over found positions to count strings whose length is greater than 0
	for (y = 0; y < found_delimeters_count + 1; y++)
	{
		int substr_beginning = substring_positions[y * 2];
		int substr_end = substring_positions[y * 2 + 1];
		int string_length = substr_end - substr_beginning;

		if (string_length > 0)
		{
			actual_number_of_strings++;
		}
	}

	if (actual_number_of_strings > 0)
	{
		result_array = (char **)memory_manager__alloc(sizeof(char *) * (nuint)actual_number_of_strings);

		//from positions of found target strings, decude positions of strings that will be part of resu�ting array of substrings

		int current_index = 0;
		y = 0;
		for (y = 0; y < actual_number_of_strings; y++)
		{
			int substr_beginning = substring_positions[y * 2];
			int substr_end = substring_positions[y * 2 + 1];
			int string_length = substr_end - substr_beginning;

			// printf("%s%d%s","beginning ", substr_beginning, "\n");
			// printf("%s%d%s","end ", substr_end, "\n");

			if (string_length > 0)
			{
				// printf("%s%d%s","string length ->", string_length, "\n");
				char *s1 = (char *)memory_manager__alloc(sizeof(char) * (nuint)string_length + 1);
				int k;
				for (k = 0; k < string_length; k++)
				{
					char the_found_char = string_to_split[substr_beginning + k];
					s1[k] = the_found_char;
					//printf("%c", the_found_char);
				}

				s1[string_length] = 0; //add null terminator
				result_array[current_index] = s1;
				//   printf(result_array[current_index]);
				// printf("\n");
				current_index++;
			}
		}

		//value is changed in pointer that was passed to the function
		*splitted_substrings_count = actual_number_of_strings;
	}
	memory_manager__free(substring_positions, TRUE);
	return result_array;
}

boole clib__string_to_boole(char *string)
{
	if (string[0] == '1')
	{
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

static char *_clib_internal__trim(char *str)
{
	while (*str == ' ' || *str == '\t')
	{
		str++;
	}

	char *start = str;

	if (!(*str))
	{
		return str;
	}

	char *end = str;

	while (*str)
	{
		if (*str != ' ' && *str != '\t')
		{
			end = str;
		}
		str++;
	}

	*(end + 1) = 0;

	return start;
}

float clib__atof(char *pstr)
{
	char *str;
	// znamienko floatu, nastavy sa na -1 ked je prvy znak "-"

	int sign = 1;

	long long integer = 0;

	double decimal = 0;

	double dec_num = 0.1;

	boole has_dot = FALSE;

	boole has_integer = FALSE;

	char p_str_copy[64];

	if (pstr == 0)
	{
		return 0;
	}

	memset(p_str_copy, 0, sizeof(p_str_copy));

	int string_length = clib__string_length(pstr);

	clib__custom_memory_copy((ubyte *)pstr, (ubyte *)p_str_copy, 0, 0, string_length);

	str = _clib_internal__trim(p_str_copy);
	//v tomto bode je kopia stringu predaneho funkci ulozena do str

	if (*str == 0)
	{
		return 0;
	}

	if (*str == '+' || *str == '-')
	{
		if (*str == '-')
		{
			sign = -1;
		}
		str++;
	}

	while (*str)
	{
		if (*str >= '0' && *str <= '9')
		{
			if (!has_dot)
			{
				if (!has_integer)
				{
					has_integer = 1;
				}

				integer *= 10;
				integer += (unsigned long long)(*str - '0');

				if (integer > (long long)INT_MAX)
				{
					//fail, integer je vacsi ako FLOAT_MAX
					return 0;
				}
			}
			else if (!has_integer)
			{
				//ked za desatinnou ciarkou (bodkou) alebo pred nou nieje ziadne cislo
				//napriklad .123 alebo 123.
				return 0;
			}
			else
			{
				if (dec_num < (double)1e-10)
				{
					//prilis vela desatinnych miest?
				}
				else
				{
					decimal += (*str - '0') * dec_num;
					dec_num *= 0.1;
				}
			}
		}
		else if (*str == '.')
		{
			if (has_dot)
			{
				//string ma viac nez jednu bodku. napriklad 1214..124
				return 0;
			}
			has_dot = 1;
		}
		else
		{
			//string obsahuje nepovolene znaky. Povolene znaky 0~9 "+" , "-", "."
			return 0;
		}
		str++;
	}

	if (has_dot && (!has_integer || dec_num == 0.1))
	{
		//za alebo pred desatinnou ciarkou sa nenachazda cislica. napriklad .124 alebo 124. alebo .
		return 0;
	}

	float ret = (float)integer + (float)decimal;
	return ret * sign;
}

void clib__str_to_float(float f, char *str, ubyte precision)
{
	float ff;
	int a, b, c, k, l = 0, m, i = 0;
	ff = f;

	// check for negetive float
	if (f < 0.0)
	{
		str[i++] = '-';
		f *= -1;
	}

	a = (int)f; // extracting whole number
	f -= a; // extracting decimal part
	k = 0;

	// number of digits in whole number
	while (1)
	{
		l = (int)clib__pow(10, k);
		m = a / l;
		if (m == 0)
		{
			break;
		}
		k++;
	}

	k--;
	// number of digits in whole number are k+1

	/*
    extracting most significant digit i.e. right most digit , and concatenating to string
    obtained as quotient by dividing number by 10^k where k = (number of digit -1)
    */

	for (l = k + 1; l > 0; l--)
	{
		b = (int)clib__pow(10, l - 1);
		c = a / b;
		str[i++] = c + 48;
		a %= b;
	}
	if (precision != 0)
	{
		str[i++] = '.';
	}

	/* extracting decimal digits till precision */

	for (l = 0; l < precision; l++)
	{
		f *= 10.0;
		b = (int)f;
		str[i++] = b + 48;
		f -= b;
	}

	str[i] = '\0';
}

static double bp[] = { 1.0, 1.5, },
dp_h[] = { 0.0, 5.84962487220764160156e-01, }, /* 0x3FE2B803, 0x40000000 */
dp_l[] = { 0.0, 1.35003920212974897128e-08, }, /* 0x3E4CFDEB, 0x43CFD006 */
zero = 0.0,
two = 2.0,
two53 = 9007199254740992.0,	/* 0x43400000, 0x00000000 */
huge = 1.0e300,
/* poly coefs for (3/2)*(log(x)-2s-2/3*s**3 */
L1 = 5.99999999999994648725e-01, /* 0x3FE33333, 0x33333303 */
L2 = 4.28571428578550184252e-01, /* 0x3FDB6DB6, 0xDB6FABFF */
L3 = 3.33333329818377432918e-01, /* 0x3FD55555, 0x518F264D */
L4 = 2.72728123808534006489e-01, /* 0x3FD17460, 0xA91D4101 */
L5 = 2.30660745775561754067e-01, /* 0x3FCD864A, 0x93C9DB65 */
L6 = 2.06975017800338417784e-01, /* 0x3FCA7E28, 0x4A454EEF */
P1 = 1.66666666666666019037e-01, /* 0x3FC55555, 0x5555553E */
P2 = -2.77777777770155933842e-03, /* 0xBF66C16C, 0x16BEBD93 */
P3 = 6.61375632143793436117e-05, /* 0x3F11566A, 0xAF25DE2C */
P4 = -1.65339022054652515390e-06, /* 0xBEBBBD41, 0xC5D26BF1 */
P5 = 4.13813679705723846039e-08, /* 0x3E663769, 0x72BEA4D0 */
lg2 = 6.93147180559945286227e-01, /* 0x3FE62E42, 0xFEFA39EF */
lg2_h = 6.93147182464599609375e-01, /* 0x3FE62E43, 0x00000000 */
lg2_l = -1.90465429995776804525e-09, /* 0xBE205C61, 0x0CA86C39 */
ovt = 8.0085662595372944372e-0017, /* -(1024-log2(ovfl+.5ulp)) */
cp = 9.61796693925975554329e-01, /* 0x3FEEC709, 0xDC3A03FD =2/(3ln2) */
cp_h = 9.61796700954437255859e-01, /* 0x3FEEC709, 0xE0000000 =(float)cp */
cp_l = -7.02846165095275826516e-09, /* 0xBE3E2FE0, 0x145B01F5 =tail of cp_h*/
ivln2 = 1.44269504088896338700e+00, /* 0x3FF71547, 0x652B82FE =1/ln2 */
ivln2_h = 1.44269502162933349609e+00, /* 0x3FF71547, 0x60000000 =24b 1/ln2*/
ivln2_l = 1.92596299112661746887e-08, /* 0x3E54AE0B, 0xF85DDF44 =1/ln2 tail*/
two54 = 1.80143985094819840000e+16, /* 0x43500000, 0x00000000 */
twom54 = 5.55111512312578270212e-17, /* 0x3C900000, 0x00000000 */
tiny = 1.0e-300,
one = 1.0;

static double pi = 3.14159265358979311600e+00, /* 0x400921FB, 0x54442D18 */
	pio2_hi = 1.57079632679489655800e+00, /* 0x3FF921FB, 0x54442D18 */
	pio2_lo = 6.12323399573676603587e-17, /* 0x3C91A626, 0x33145C07 */
	pS0 = 1.66666666666666657415e-01, /* 0x3FC55555, 0x55555555 */
	pS1 = -3.25565818622400915405e-01, /* 0xBFD4D612, 0x03EB6F7D */
	pS2 = 2.01212532134862925881e-01, /* 0x3FC9C155, 0x0E884455 */
	pS3 = -4.00555345006794114027e-02, /* 0xBFA48228, 0xB5688F3B */
	pS4 = 7.91534994289814532176e-04, /* 0x3F49EFE0, 0x7501B288 */
	pS5 = 3.47933107596021167570e-05, /* 0x3F023DE1, 0x0DFDF709 */
	qS1 = -2.40339491173441421878e+00, /* 0xC0033A27, 0x1C8A2D4B */
	qS2 = 2.02094576023350569471e+00, /* 0x40002AE5, 0x9C598AC8 */
	qS3 = -6.88283971605453293030e-01, /* 0xBFE6066C, 0x1B8D0159 */
	qS4 = 7.70381505559019352791e-02; /* 0x3FB3B8C5, 0xB12E9282 */

static double copysign_c(double x, double y)
{
	if ((y < 0 && x > 0) || (y > 0 && x < 0))
	{
		return -x;
	}
	else
	{
		return x;
	}
}

double scalbn(double x, int n)
{
	int k, hx, lx;
	hx = __HI(x);
	lx = __LO(x);
	k = (hx & 0x7ff00000) >> 20; /* extract exponent */
	if (k == 0)
	{ /* 0 or subnormal x */
		if ((lx | (hx & 0x7fffffff)) == 0)
		{
			return x; /* +-0 */
		}
		x *= two54;
		hx = __HI(x);
		k = ((hx & 0x7ff00000) >> 20) - 54;
		if (n < -50000)
		{
			return tiny * x; /*underflow*/
		}
	}
	if (k == 0x7ff)
	{
		return x + x; /* NaN or Inf */
	}
	k = k + n;
	if (k > 0x7fe)
	{
		return huge * copysign_c(huge, x); /* overflow  */
	}
	if (k > 0) /* normal result */
	{
		__HI(x) = (hx & (int)0x800fffff) | (k << 20);
		return x;
	}
	if (k <= -54)
	{
		if (n > 50000)
		{ /* in case integer overflow in n+k */
			return huge * copysign_c(huge, x); /*overflow*/
		}
		else
		{
			return tiny * copysign_c(tiny, x); /*underflow*/
		}
	}

	k += 54; /* subnormal result */
	__HI(x) = (hx & (int)0x800fffff) | (k << 20);
	return x * twom54;
}

void clib__ftoa(float number, char *str)
{
	int precision = 6;

	float f = number;
	int a, b, c, k, l = 0, m, i = 0;

	// check for negetive float
	if (f < 0.0f)
	{
		str[i++] = '-';
		f *= -1;
	}

	a = (int)f; // extracting whole number
	f -= (float)a; // extracting decimal part
	k = precision;

	// number of digits in whole number
	while (k > -1)
	{
		l = (int)(float)clib__pow(10, k);
		m = a / l;
		if (m > 0)
		{
			break;
		}
		k--;
	}

	// number of digits in whole number are k+1

	/*
    extracting most significant digit i.e. right most digit , and concatenating to string
    obtained as quotient by dividing number by 10^k where k = (number of digit -1)
    */

	if (number < 1.f)
	{
		str[i++] = '0';
	}

	for (l = k + 1; l > 0; l--)
	{
		b = (int)(float)clib__pow(10, l - 1);
		c = a / b;
		str[i++] = (char)(c + 48);
		a %= b;
	}
	str[i++] = '.';

	/* extracting decimal digits till precision */

	for (l = 0; l < precision; l++)
	{
		f *= 10.0f;
		b = (int)f;
		str[i++] = (char)(b + 48);
		f -= (float)b;
	}
	str[i] = '\0';
}

void clib__ftoa_1(float number, char *str, int precision)
{
	float f = number;
	int a, b, c, k, l = 0, m, i = 0;

	// check for negetive float
	if (f < 0.0f)
	{
		str[i++] = '-';
		f *= -1;
	}

	a = (int)f; // extracting whole number
	f -= (float)a; // extracting decimal part
	k = precision;

	// number of digits in whole number
	while (k > -1)
	{
		l = (int)(float)clib__pow(10, k);
		m = a / l;
		if (m > 0)
		{
			break;
		}
		k--;
	}

	// number of digits in whole number are k+1

	/*
    extracting most significant digit i.e. right most digit , and concatenating to string
    obtained as quotient by dividing number by 10^k where k = (number of digit -1)
    */

	if (number < 1.f)
	{
		str[i++] = '0';
	}

	for (l = k + 1; l > 0; l--)
	{
		b = (int)(float)clib__pow(10, l - 1);
		c = a / b;
		str[i++] = (char)(c + 48);
		a %= b;
	}
	str[i++] = '.';

	/* extracting decimal digits till precision */

	for (l = 0; l < precision; l++)
	{
		f *= 10.0f;
		b = (int)f;
		str[i++] = (char)(b + 48);
		f -= (float)b;
	}
	str[i] = '\0';
}

double clib__atan(double x)
{
	static double atanhi[] = {
		4.63647609000806093515e-01, /* atan(0.5)hi 0x3FDDAC67, 0x0561BB4F */
		7.85398163397448278999e-01, /* atan(1.0)hi 0x3FE921FB, 0x54442D18 */
		9.82793723247329054082e-01, /* atan(1.5)hi 0x3FEF730B, 0xD281F69B */
		1.57079632679489655800e+00, /* atan(inf)hi 0x3FF921FB, 0x54442D18 */
	};

	static double atanlo[] = {
		2.26987774529616870924e-17, /* atan(0.5)lo 0x3C7A2B7F, 0x222F65E2 */
		3.06161699786838301793e-17, /* atan(1.0)lo 0x3C81A626, 0x33145C07 */
		1.39033110312309984516e-17, /* atan(1.5)lo 0x3C700788, 0x7AF0CBBD */
		6.12323399573676603587e-17, /* atan(inf)lo 0x3C91A626, 0x33145C07 */
	};

	static double aT[] = {
		3.33333333333329318027e-01, /* 0x3FD55555, 0x5555550D */
		-1.99999999998764832476e-01, /* 0xBFC99999, 0x9998EBC4 */
		1.42857142725034663711e-01, /* 0x3FC24924, 0x920083FF */
		-1.11111104054623557880e-01, /* 0xBFBC71C6, 0xFE231671 */
		9.09088713343650656196e-02, /* 0x3FB745CD, 0xC54C206E */
		-7.69187620504482999495e-02, /* 0xBFB3B0F2, 0xAF749A6D */
		6.66107313738753120669e-02, /* 0x3FB10D66, 0xA0D03D51 */
		-5.83357013379057348645e-02, /* 0xBFADDE2D, 0x52DEFD9A */
		4.97687799461593236017e-02, /* 0x3FA97B4B, 0x24760DEB */
		-3.65315727442169155270e-02, /* 0xBFA2B444, 0x2C6A6C2F */
		1.62858201153657823623e-02, /* 0x3F90AD3A, 0xE322DA11 */
	};

	static double one = 1.0, huge = 1.0e300;

	double w, s1, s2, z;
	int ix, hx, id;

	hx = __HI(x);
	ix = hx & 0x7fffffff;
	if (ix >= 0x44100000)
	{ /* if |x| >= 2^66 */
		if (ix > 0x7ff00000 || (ix == 0x7ff00000 && (__LO(x) != 0)))
		{
			return x + x; /* NaN */
		}
		if (hx > 0)
		{
			return atanhi[3] + atanlo[3];
		}
		else
		{
			return -atanhi[3] - atanlo[3];
		}
	}
	if (ix < 0x3fdc0000)
	{ /* |x| < 0.4375 */
		if (ix < 0x3e200000)
		{ /* |x| < 2^-29 */
			if (huge + x > one)
			{
				return x; /* raise inexact */
			}
		}
		id = -1;
	}
	else
	{
		x = clib__dabs(x);
		if (ix < 0x3ff30000)
		{ /* |x| < 1.1875 */
			if (ix < 0x3fe60000)
			{ /* 7/16 <=|x|<11/16 */
				id = 0;
				x = (2.0 * x - one) / (2.0 + x);
			}
			else
			{ /* 11/16<=|x|< 19/16 */
				id = 1;
				x = (x - one) / (x + one);
			}
		}
		else
		{
			if (ix < 0x40038000)
			{ /* |x| < 2.4375 */
				id = 2;
				x = (x - 1.5) / (one + 1.5 * x);
			}
			else
			{ /* 2.4375 <= |x| < 2^66 */
				id = 3;
				x = -1.0 / x;
			}
		}
	}
	/* end of argument reduction */
	z = x * x;
	w = z * z;
	/* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
	s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
	s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
	if (id < 0)
	{
		return x - x * (s1 + s2);
	}
	else
	{
		z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
		return (hx < 0) ? -z : z;
	}
}

double clib__atan2(double y, double x)
{
	static double tiny = 1.0e-300, zero = 0.0, pi_o_4 = 7.8539816339744827900E-01, /* 0x3FE921FB, 0x54442D18 */
		pi_o_2 = 1.5707963267948965580E+00, /* 0x3FF921FB, 0x54442D18 */
		pi = 3.1415926535897931160E+00, /* 0x400921FB, 0x54442D18 */
		pi_lo = 1.2246467991473531772E-16; /* 0x3CA1A626, 0x33145C07 */

	double z;
	int k, m, hx, hy, ix, iy;
	int lx, ly;

	hx = __HI(x);
	ix = hx & 0x7fffffff;
	lx = __LO(x);
	hy = __HI(y);
	iy = hy & 0x7fffffff;
	ly = __LO(y);
	if (((ix | ((lx | -lx) >> 31)) > 0x7ff00000) || ((iy | ((ly | -ly) >> 31)) > 0x7ff00000)) /* x or y is NaN */
	{
		return x + y;
	}
	if ((hx - 0x3ff00000 | lx) == 0)
	{
		return clib__atan(y); /* x=1.0 */
	}
	m = ((hy >> 31) & 1) | ((hx >> 30) & 2); /* 2*sign(x)+sign(y) */

	/* when y = 0 */
	if ((iy | ly) == 0)
	{
		switch (m)
		{
		case 0:
		case 1:
			return y; /* atan(+-0,+anything)=+-0 */
		case 2:
			return pi + tiny; /* atan(+0,-anything) = pi */
		case 3:
			return -pi - tiny; /* atan(-0,-anything) =-pi */
		}
	}
	/* when x = 0 */
	if ((ix | lx) == 0)
	{
		return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;
	}

	/* when x is INF */
	if (ix == 0x7ff00000)
	{
		if (iy == 0x7ff00000)
		{
			switch (m)
			{
			case 0:
				return pi_o_4 + tiny; /* atan(+INF,+INF) */
			case 1:
				return -pi_o_4 - tiny; /* atan(-INF,+INF) */
			case 2:
				return 3.0 * pi_o_4 + tiny; /*atan(+INF,-INF)*/
			case 3:
				return -3.0 * pi_o_4 - tiny; /*atan(-INF,-INF)*/
			}
		}
		else
		{
			switch (m)
			{
			case 0:
				return zero; /* atan(+...,+INF) */
			case 1:
				return -zero; /* atan(-...,+INF) */
			case 2:
				return pi + tiny; /* atan(+...,-INF) */
			case 3:
				return -pi - tiny; /* atan(-...,-INF) */
			}
		}
	}
	/* when y is INF */
	if (iy == 0x7ff00000)
	{
		return (hy < 0) ? -pi_o_2 - tiny : pi_o_2 + tiny;
	}

	/* compute y/x */
	k = (iy - ix) >> 20;
	if (k > 60)
	{
		z = pi_o_2 + 0.5 * pi_lo; /* |y/x| >  2**60 */
	}
	else if (hx < 0 && k < -60)
	{
		z = 0.0; /* |y|/x < -2**60 */
	}
	else
	{
		z = clib__atan(clib__dabs(y / x)); /* safe to do y/x */
	}
	switch (m)
	{
	case 0:
		return z; /* atan(+,+) */
	case 1:
		__HI(z) ^= 0x80000000;
		return z; /* atan(-,+) */
	case 2:
		return pi - (z - pi_lo); /* atan(+,-) */
	default: /* case 3 */
		return (z - pi_lo) - pi; /* atan(-,-) */
	}
}
//

double clib__pow(double x, double y)
{
	//printf("lul \n");
	double z, ax, z_h, z_l, p_h, p_l;
	double y1, t1, t2, r, s, t, u, v, w;
	int i0, i1, i, j, k, yisint, n;
	int hx, hy, ix, iy;
	unsigned lx, ly;

	i0 = ((*(int *)&one) >> 29) ^ 1;
	i1 = 1 - i0;
	hx = __HI(x);
	lx = (uint)__LO(x);
	hy = __HI(y);
	ly = (uint)__LO(y);
	ix = hx & 0x7fffffff;
	iy = hy & 0x7fffffff;

	/* y==zero: x**0 = 1 */
	if (((uint)iy | ly) == 0)
	{
		//printf("return one");
		return one;
	}
	/* +-NaN return x+y */
	if (ix > 0x7ff00000 || ((ix == 0x7ff00000) && (lx != 0)) || iy > 0x7ff00000 || ((iy == 0x7ff00000) && (ly != 0)))
	{
		return x + y;
	}

	/* determine if y is an odd int when x < 0
     * yisint = 0	... y is not an integer
     * yisint = 1	... y is an odd int
     * yisint = 2	... y is an even int
     */
	yisint = 0;
	if (hx < 0)
	{
		if (iy >= 0x43400000)
		{
			yisint = 2; /* even integer y */
		}
		else if (iy >= 0x3ff00000)
		{
			k = (iy >> 20) - 0x3ff; /* exponent */
			if (k > 20)
			{
				j = (int)ly >> (52 - k);
				if ((j << (52 - k)) == (int)ly)
				{
					yisint = 2 - (j & 1);
				}
			}
			else if (ly == 0)
			{
				j = iy >> (20 - k);
				if ((j << (20 - k)) == iy)
				{
					yisint = 2 - (j & 1);
				}
			}
		}
	}

	/* special value of y */
	if (ly == 0)
	{
		if (iy == 0x7ff00000)
		{ /* y is +-inf */
			if (((ix - (int)0x3ff00000) | (int)lx) == 0)
			{
				return y - y; /* inf**+-1 is NaN */
			}
			else if (ix >= 0x3ff00000) /* (|x|>1)**+-inf = inf,0 */
			{
				return (hy >= 0) ? y : zero;
			}
			else /* (|x|<1)**-,+inf = inf,0 */
			{
				return (hy < 0) ? -y : zero;
			}
		}
		if (iy == 0x3ff00000)
		{ /* y is  +-1 */
			if (hy < 0)
			{
				return one / x;
			}
			else
			{
				return x;
			}
		}
		if (hy == 0x40000000)
		{
			return x * x; /* y is  2 */
		}
		if (hy == 0x3fe00000)
		{ /* y is  0.5 */
			if (hx >= 0) /* x >= +0 */
			{
				return clib__sqrt(x);
			}
		}
	}

	ax = clib__dabs(x);
	/* special value of x */
	if (lx == 0)
	{
		if (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000)
		{
			z = ax; /*x is +-0,+-inf,+-1*/
			if (hy < 0)
			{
				z = one / z; /* z = (1/|x|) */
			}
			if (hx < 0)
			{
				if (((ix - 0x3ff00000) | yisint) == 0)
				{
					z = (z - z) / (z - z); /* (-1)**non-int is NaN */
				}
				else if (yisint == 1)
				{
					z = -z; /* (x<0)**odd = -(|x|**odd) */
				}
			}
			return z;
		}
	}

	n = (hx >> 31) + 1;

	/* (x<0)**(non-int) is NaN */
	if ((n | yisint) == 0)
	{
		return (x - x) / (x - x);
	}

	s = one; /* s (sign of result -ve**odd) = -1 else = 1 */
	if ((n | (yisint - 1)) == 0)
	{
		s = -one; /* (-ve)**(odd int) */
	}

	/* |y| is huge */
	if (iy > 0x41e00000)
	{ /* if |y| > 2**31 */
		if (iy > 0x43f00000)
		{ /* if |y| > 2**64, must o/uflow */
			if (ix <= 0x3fefffff)
			{
				return (hy < 0) ? huge * huge : tiny * tiny;
			}
			if (ix >= 0x3ff00000)
			{
				return (hy > 0) ? huge * huge : tiny * tiny;
			}
		}
		/* over/underflow if x is not close to one */
		if (ix < 0x3fefffff)
		{
			return (hy < 0) ? s * huge * huge : s * tiny * tiny;
		}
		if (ix > 0x3ff00000)
		{
			return (hy > 0) ? s * huge * huge : s * tiny * tiny;
		}
		/* now |1-x| is tiny <= 2**-20, suffice to compute
           log(x) by x-x^2/2+x^3/3-x^4/4 */
		t = ax - one; /* t has 20 trailing zeros */
		w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
		u = ivln2_h * t; /* ivln2_h has 21 sig. bits */
		v = t * ivln2_l - w * ivln2;
		t1 = u + v;
		__LO(t1) = 0;
		t2 = v - (t1 - u);
	}
	else
	{
		double ss, s2, s_h, s_l, t_h, t_l;
		n = 0;
		/* take care subnormal number */
		if (ix < 0x00100000)
		{
			ax *= two53;
			n -= 53;
			ix = __HI(ax);
		}
		n += ((ix) >> 20) - 0x3ff;
		j = ix & 0x000fffff;
		/* determine interval */
		ix = j | 0x3ff00000; /* normalize ix */
		if (j <= 0x3988E)
		{
			k = 0; /* |x|<sqrt(3/2) */
		}
		else if (j < 0xBB67A)
		{
			k = 1; /* |x|<sqrt(3)   */
		}
		else
		{
			k = 0;
			n += 1;
			ix -= 0x00100000;
		}
		__HI(ax) = ix;

		/* compute ss = s_h+s_l = (x-1)/(x+1) or (x-1.5)/(x+1.5) */
		u = ax - bp[k]; /* bp[0]=1.0, bp[1]=1.5 */
		v = one / (ax + bp[k]);
		ss = u * v;
		s_h = ss;
		__LO(s_h) = 0;
		/* t_h=ax+bp[k] High */
		t_h = zero;
		__HI(t_h) = ((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18);
		t_l = ax - (t_h - bp[k]);
		s_l = v * ((u - s_h * t_h) - s_h * t_l);
		/* compute log(ax) */
		s2 = ss * ss;
		r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
		r += s_l * (s_h + ss);
		s2 = s_h * s_h;
		t_h = 3.0 + s2 + r;
		__LO(t_h) = 0;
		t_l = r - ((t_h - 3.0) - s2);
		/* u+v = ss*(1+...) */
		u = s_h * t_h;
		v = s_l * t_h + t_l * ss;
		/* 2/(3log2)*(ss+...) */
		p_h = u + v;
		__LO(p_h) = 0;
		p_l = v - (p_h - u);
		z_h = cp_h * p_h; /* cp_h+cp_l = 2/(3*log2) */
		z_l = cp_l * p_h + p_l * cp + dp_l[k];
		/* log2(ax) = (ss+..)*2/(3*log2) = n + dp_h + z_h + z_l */
		t = (double)n;
		t1 = (((z_h + z_l) + dp_h[k]) + t);
		__LO(t1) = 0;
		t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
	}

	/* split up y into y1+y2 and compute (y1+y2)*(t1+t2) */
	y1 = y;
	__LO(y1) = 0;
	p_l = (y - y1) * t1 + y * t2;
	p_h = y1 * t1;
	z = p_l + p_h;
	j = __HI(z);
	i = __LO(z);
	if (j >= 0x40900000)
	{ /* z >= 1024 */
		if (((j - 0x40900000) | i) != 0) /* if z > 1024 */
		{
			return s * huge * huge; /* overflow */
		}
		else
		{
			if (p_l + ovt > z - p_h)
			{
				return s * huge * huge; /* overflow */
			}
		}
	}
	else if ((j & 0x7fffffff) >= 0x4090cc00)
	{ /* z <= -1075 */
		if (((j - (int)0xc090cc00) | (int)i) != 0)
		{ /* z < -1075 */
			return s * tiny * tiny;
		} /* underflow */
		else
		{
			if (p_l <= z - p_h)
			{
				return s * tiny * tiny; /* underflow */
			}
		}
	}
	/*
     * compute 2**(p_h+p_l)
     */
	i = j & 0x7fffffff;
	k = (i >> 20) - 0x3ff;
	n = 0;
	if (i > 0x3fe00000)
	{ /* if |z| > 0.5, set n = [z+0.5] */
		n = j + (0x00100000 >> (k + 1));
		k = ((n & 0x7fffffff) >> 20) - 0x3ff; /* new k for n */
		t = zero;
		__HI(t) = (n & ~(0x000fffff >> k));
		n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
		if (j < 0)
		{
			n = -n;
		}
		p_h -= t;
	}
	t = p_l + p_h;
	__LO(t) = 0;
	u = t * lg2_h;
	v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
	z = u + v;
	w = v - (z - u);
	t = z * z;
	t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
	r = (z * t1) / (t1 - two) - (w + z * w);
	z = one - (r - z);
	j = __HI(z);
	j += (n << 20);
	if ((j >> 20) <= 0)
	{
		z = scalbn(z, n); /* subnormal output */
	}
	else
	{
		__HI(z) += (n << 20);
	}
	//printf("%s %f %s", "s ", s, "\n");
	//printf("%s %f %s", "z ", z, "\n");

	return s * z;
}

void clib__split_str_must_call_after_usage(int number_of_strings, char **strings)
{
	for (int i = 0; i < number_of_strings; i++)
	{
		int string_length = clib__string_length(strings[i]);
		memset(strings[i], 0, string_length);
		memory_manager__free(strings[i], TRUE);
	}

	memory_manager__free(strings, TRUE);
}

void clib__merge_wstr(ubyte *first, ubyte *second)
{
	int first_string_length = 0;
	int first_string_current_index = 0;

	int second_string_length = 0;
	int second_string_current_index = 0;

	for (;;)
	{
		/*    std::cout << std::hex << std::endl;
            std::cout << "x: " << first_string_current_index << " : " << (uint)first[first_string_current_index] << " : " << (uint)first[first_string_current_index + 1];*/

		//unicode string is, or should, be terminated by two zeroes.
		//to find out where unicode string ends, string address space must be scanned from the start of the string
		//the code must check if one of the null bytes does not belong to some other character. Because unicode chars also contain null bytes
		//only if two null bytes both belong to same character, can end of the string considered to be found

		//better option that to check whether two null bytes belong to same wide char would be to only check bytes from same wide char

		if (first[first_string_current_index] == 0 && first[first_string_current_index + 1] == 0)
		{
			first_string_length = first_string_current_index;

			/*      std::cout << std::endl;
                  std::cout << "first_string_length " << first_string_length << std::endl;
                  std::cout << "first_string_length hex " << std::hex << first_string_length << std::endl;*/

			break;
		}
		else
		{
			first_string_current_index += 2;
		}
	}

	for (;;)
	{
		if (second[second_string_current_index] == 0 && second[second_string_current_index + 1] == 0)
		{
			second_string_length = second_string_current_index;
			/*       std::cout << "second_string_length " << second_string_length << std::endl;
                   std::cout << "second_string_length hex " << std::hex << second_string_length << std::endl;*/

			break;
		}
		second_string_current_index += 2;
	}

	ubyte *first_address = (ubyte *)first + first_string_length;
	ubyte *second_address = (ubyte *)second;

	memcpy((void *)first_address, (void *)second_address, (nuint)second_string_length);

	//it is assumed that there is aditional space available in the buffer of first string
	//place in RAM where unicode string ends is null terminated by two strings not one
	//but that is wrong to assume. you dumb dumb
	//null out aditional two bytes in wide string, so resulting string is null terminated
	first_address[second_string_length] = 0;
	first_address[second_string_length + 1] = 0;
}

int clib__abs(int a)
{
	if (a < 0)
	{
		a = -a;
	}
	return a;
}

double clib__dabs(double x)
{
	__HI(x) &= 0x7fffffff;
	return x;
}

double clib__sqrt(double x)
{
	double z;
	int sign = (int)0x80000000;
	unsigned r, t1, s1, ix1, q1;
	int ix0, s0, q, m, t, i;
	int blabla; //pridal som jal
	ix0 = __HI(x); /* high word of x */
	ix1 = (uint)__LO(x); /* low word of x */

	/* take care of Inf and NaN */
	if ((ix0 & 0x7ff00000) == 0x7ff00000)
	{
		return x * x + x; /* sqrt(NaN)=NaN, sqrt(+inf)=+inf
                       sqrt(-inf)=sNaN */
	}
	/* take care of zero */
	if (ix0 <= 0)
	{
		blabla = ix0 & (~sign);
		if (((uint)blabla | ix1) == 0)
		{
			return x;
		} /* sqrt(+-0) = +-0 */
		else if (ix0 < 0)
		{
			return (x - x) / (x - x);
		} /* sqrt(-ve) = sNaN */
	}
	/* normalize x */
	m = (ix0 >> 20);
	if (m == 0)
	{ /* subnormal x */
		while (ix0 == 0)
		{
			m -= 21;
			ix0 |= (ix1 >> 11);
			ix1 <<= 21;
		}
		for (i = 0; (ix0 & 0x00100000) == 0; i++)
		{
			ix0 <<= 1;
		}
		m -= i - 1;
		ix0 |= (ix1 >> (32 - i));
		ix1 <<= i;
	}
	m -= 1023; /* unbias exponent */
	ix0 = (ix0 & 0x000fffff) | 0x00100000;
	if (m & 1)
	{ /* odd m, double x to make it even */
		ix0 += (int)(ix0 + (int)((ix1 & (uint)sign) >> 31));
		ix1 += ix1;
	}
	m >>= 1; /* m = [m/2] */

	/* generate sqrt(x) bit by bit */
	ix0 += ix0 + (int)((ix1 & (uint)sign) >> 31);
	ix1 += ix1;
	q = q1 = s0 = s1 = 0; /* [q,q1] = sqrt(x) */
	r = 0x00200000; /* r = moving bit from right to left */

	while (r != 0)
	{
		t = s0 + (int)r;
		if (t <= ix0)
		{
			s0 = t + (int)r;
			ix0 -= t;
			q += r;
		}
		ix0 += ix0 + (int)((ix1 & (uint)sign) >> 31);
		ix1 += ix1;
		r >>= 1;
	}

	r = (uint)sign;
	while (r != 0)
	{
		t1 = s1 + r;
		t = s0;
		if ((t < ix0) || ((t == ix0) && (t1 <= ix1)))
		{
			s1 = t1 + r;
			if (((t1 & (uint)sign) == (uint)sign) && (s1 & (uint)sign) == 0)
			{
				s0 += 1;
			}
			ix0 -= t;
			if (ix1 < t1)
			{
				ix0 -= 1;
			}
			ix1 -= t1;
			q1 += r;
		}
		ix0 += ix0 + (int)((ix1 & (uint)sign) >> 31);
		ix1 += ix1;
		r >>= 1;
	}

	/* use floating add to find out rounding direction */
	if ((ix0 | (int)ix1) != 0)
	{
		z = one - tiny; /* trigger inexact flag */
		if (z >= one)
		{
			z = one + tiny;
			if (q1 == (unsigned)0xffffffff)
			{
				q1 = 0;
				q += 1;
			}
			else if (z > one)
			{
				if (q1 == (unsigned)0xfffffffe)
				{
					q += 1;
				}
				q1 += 2;
			}
			else
			{
				q1 += (q1 & 1);
			}
		}
	}
	ix0 = (q >> 1) + 0x3fe00000;
	ix1 = q1 >> 1;
	if ((q & 1) == 1)
	{
		ix1 |= (uint)sign;
	}
	ix0 += (m << 20);
	__HI(z) = ix0;
	__LO(z) = (int)ix1;
	return z;
}

float clib__sqrtf(float input)
{
	double x = (double)input;
	double z;
	int sign = (int)0x80000000;
	unsigned r, t1, s1, ix1, q1;
	int ix0, s0, q, m, t, i;
	int blabla; //pridal som jal
	ix0 = __HI(x); /* high word of x */
	ix1 = (uint)__LO(x); /* low word of x */

	/* take care of Inf and NaN */
	if ((ix0 & 0x7ff00000) == 0x7ff00000)
	{
		return (float)(x * x + x); /* sqrt(NaN)=NaN, sqrt(+inf)=+inf
                       sqrt(-inf)=sNaN */
	}
	/* take care of zero */
	if (ix0 <= 0)
	{
		blabla = ix0 & (~sign);
		if (((uint)blabla | ix1) == 0)
		{
			return (float)x;
		} /* sqrt(+-0) = +-0 */
		else if (ix0 < 0)
		{
			return (float)((x - x) / (x - x));
		} /* sqrt(-ve) = sNaN */
	}
	/* normalize x */
	m = (ix0 >> 20);
	if (m == 0)
	{ /* subnormal x */
		while (ix0 == 0)
		{
			m -= 21;
			ix0 |= (ix1 >> 11);
			ix1 <<= 21;
		}
		for (i = 0; (ix0 & 0x00100000) == 0; i++)
		{
			ix0 <<= 1;
		}
		m -= i - 1;
		ix0 |= (ix1 >> (32 - i));
		ix1 <<= i;
	}
	m -= 1023; /* unbias exponent */
	ix0 = (ix0 & 0x000fffff) | 0x00100000;
	if (m & 1)
	{ /* odd m, double x to make it even */
		ix0 += (int)(ix0 + (int)((ix1 & (uint)sign) >> 31));
		ix1 += ix1;
	}
	m >>= 1; /* m = [m/2] */

	/* generate sqrt(x) bit by bit */
	ix0 += ix0 + (int)((ix1 & (uint)sign) >> 31);
	ix1 += ix1;
	q = q1 = s0 = s1 = 0; /* [q,q1] = sqrt(x) */
	r = 0x00200000; /* r = moving bit from right to left */

	while (r != 0)
	{
		t = s0 + (int)r;
		if (t <= ix0)
		{
			s0 = t + (int)r;
			ix0 -= t;
			q += r;
		}
		ix0 += ix0 + (int)((ix1 & (uint)sign) >> 31);
		ix1 += ix1;
		r >>= 1;
	}

	r = (uint)sign;
	while (r != 0)
	{
		t1 = s1 + r;
		t = s0;
		if ((t < ix0) || ((t == ix0) && (t1 <= ix1)))
		{
			s1 = t1 + r;
			if (((t1 & (uint)sign) == (uint)sign) && (s1 & (uint)sign) == 0)
			{
				s0 += 1;
			}
			ix0 -= t;
			if (ix1 < t1)
			{
				ix0 -= 1;
			}
			ix1 -= t1;
			q1 += r;
		}
		ix0 += ix0 + (int)((ix1 & (uint)sign) >> 31);
		ix1 += ix1;
		r >>= 1;
	}

	/* use floating add to find out rounding direction */
	if ((ix0 | (int)ix1) != 0)
	{
		z = one - tiny; /* trigger inexact flag */
		if (z >= one)
		{
			z = one + tiny;
			if (q1 == (unsigned)0xffffffff)
			{
				q1 = 0;
				q += 1;
			}
			else if (z > one)
			{
				if (q1 == (unsigned)0xfffffffe)
				{
					q += 1;
				}
				q1 += 2;
			}
			else
			{
				q1 += (q1 & 1);
			}
		}
	}
	ix0 = (q >> 1) + 0x3fe00000;
	ix1 = q1 >> 1;
	if ((q & 1) == 1)
	{
		ix1 |= (uint)sign;
	}
	ix0 += (m << 20);
	__HI(z) = ix0;
	__LO(z) = (int)ix1;
	return (float)z;
}

// Swaps two elements of the array given their size
static void _clib_internal__qsortswap(void *a, void *b, uint64 size)
{
	char *temp = (char *)memory_manager__alloc(size);
	memset(temp, 0, size);
	memcpy(temp, a, size);
	memcpy(a, b, size);
	memcpy(b, temp, size);
	memory_manager__free(temp, TRUE);
}

// Partition function for the quicksort algorithm
static size_t _clib_internal__qsort_partition(void *base, uint64 low, uint64 high, uint64 size, p_fnc_clib_qsort_comparator compare)
{
	void *pivot = (char *)base + high * size;
	uint64 i = low - 1;

	for (uint64 j = low; j < high; ++j)
	{
		void *current = (char *)base + j * size;
		if (compare(current, pivot) <= 0)
		{
			++i;
			_clib_internal__qsortswap((char *)base + i * size, current, size);
		}
	}

	_clib_internal__qsortswap((char *)base + (i + 1) * size, pivot, size);
	return i + 1;
}

// Quicksort algorithm
static void _clib_internal__quicksort(void *base, uint64 low, uint64 high, uint64 size, p_fnc_clib_qsort_comparator compare)
{
	if (low < high)
	{
		uint64 partition_index = _clib_internal__qsort_partition(base, low, high, size, compare);

		if (partition_index > 0)
		{
			_clib_internal__quicksort(base, low, partition_index - 1, size, compare);
		}
		_clib_internal__quicksort(base, partition_index + 1, high, size, compare);
	}
}

// Interface function for qsort
void clib__qsort(void *base, uint64 num_of_elements, uint64 size_of_element, p_fnc_clib_qsort_comparator compare)
{
	uint64 size = size_of_element;
	uint64 low = 0;
	uint64 high = num_of_elements - 1;

	_clib_internal__quicksort(base, low, high, size, compare);
}

void clib__read_string_from_memory_and_save_to_location(char *memory_to_extract_from, char *location_to_save_string_to)
{
	int curr_string_length = 0;

	//string length limit, 1000

	int i;

	for (i = 0; i < 1000; i++)
	{
		char single_char = *(memory_to_extract_from + i);
		if (single_char == 0)
		{
		}
		curr_string_length++;
	}

	if (curr_string_length != 0)
	{
		clib__copy_memory(memory_to_extract_from, location_to_save_string_to, curr_string_length, 1000);
	}
}

void clib__custom_memory_copy(ubyte *source, ubyte *destination, int source_offset, int destination_offset, int size)
{
	int x;
	for (x = 0; x < size; x++)
	{
		destination[destination_offset + x] = source[source_offset + x];
	}
}

//#pragma optimize("", off) aby visual studio nenahrazdoval moju memset funkciu standartnou memset funkciou

#ifdef ARCHITECTURE_I386

void *__cdecl memset(void *dest, int c, size_t count)
{
	//pridat dbg memset

	char *bytes = (char *)dest;
	while (count--)
	{
		*bytes++ = (char)c;
	}
	return dest;
}

void *__cdecl memcpy(void *dest, void const *src, size_t count)
{
	char *dest8 = (char *)dest;
	const char *src8 = (const char *)src;
	while (count--)
	{
		*dest8++ = *src8++;
	}
	return dest;
}

void *__cdecl memmove(void *destination, void const *source, size_t length)
{
	ubyte *_destination = (ubyte *)destination;
	unsigned const char *_source = (unsigned const char *)source;
	if (_source < _destination)
	{
		_source += length;
		_destination += length;
		while (length--)
		{
			*--_destination = *--_source;
		}
	}
	else
	{
		while (length--)
		{
			*_destination++ = *_source++;
		}
	}
	return destination;
}

#endif

#ifdef ARCHITECTURE_AMD64

void *__cdecl memset(void *dest, int c, size_t count)
{
	//pridat dbg memset

	char *bytes = (char *)dest;
	while (count--)
	{
		*bytes++ = (char)c;
	}
	return dest;
}

void *__cdecl memcpy(void *dest, void const *src, size_t count)
{
	char *dest8 = (char *)dest;
	const char *src8 = (const char *)src;
	while (count--)
	{
		*dest8++ = *src8++;
	}
	return dest;
}

void *__cdecl memmove(void *destination, void const *source, size_t length)
{
	ubyte *_destination = (ubyte *)destination;
	unsigned const char *_source = (unsigned const char *)source;
	if (_source < _destination)
	{
		_source += length;
		_destination += length;
		while (length--)
		{
			*--_destination = *--_source;
		}
	}
	else
	{
		while (length--)
		{
			*_destination++ = *_source++;
		}
	}
	return destination;
}

#endif

void clib__zerostring(char *Str)
{
	while (*Str)
	{
		*Str++ = 0;
	}
}

void clib__uint64_to_ansi_string(uint64 value, char *buffer)
{
	char temp[20];
	char *p = temp;

	do
	{
		*p++ = (char)(value % 10) + '0';
		value /= 10;
	} while (value > 0);

	do
	{
		*buffer++ = *--p;
	} while (p != temp);

	*buffer = '\0';
}

void clib__int64_to_ansi_string(int64 value, char *buffer)
{
	uint64 u = (uint64)(value);
	if (value < 0)
	{
		*buffer++ = '-';
		u = ~u + 1;
	}

	clib__uint64_to_ansi_string(u, buffer);
}

double clib__acos(double x)
{
	double z, p, q, r, w, s, c, df;
	int hx, ix;
	hx = __HI(x);
	ix = hx & 0x7fffffff;
	if (ix >= 0x3ff00000)
	{ /* |x| >= 1 */
		if (((ix - 0x3ff00000) | __LO(x)) == 0)
		{ /* |x|==1 */
			if (hx > 0)
			{
				return 0.0; /* acos(1) = 0  */
			}
			else
			{
				return pi + 2.0 * pio2_lo; /* acos(-1)= pi */
			}
		}
		return (x - x) / (x - x); /* acos(|x|>1) is NaN */
	}
	if (ix < 0x3fe00000)
	{ /* |x| < 0.5 */
		if (ix <= 0x3c600000)
		{
			return pio2_hi + pio2_lo; /*if|x|<2**-57*/
		}
		z = x * x;
		p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
		q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
		r = p / q;
		return pio2_hi - (x - (pio2_lo - x * r));
	}
	else if (hx < 0)
	{ /* x < -0.5 */
		z = (one + x) * 0.5;
		p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
		q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
		s = clib__sqrt(z);
		r = p / q;
		w = r * s - pio2_lo;
		return pi - 2.0 * (s + w);
	}
	else
	{ /* x > 0.5 */
		z = (one - x) * 0.5;
		s = clib__sqrt(z);
		df = s;
		__LO(df) = 0;
		c = (z - df * df) / (s + df);
		p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
		q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
		r = p / q;
		w = r * s + c;
		return 2.0 * (df + w);
	}
}

float clib__acosf(float input)
{
	double x = (double)input;
	double z, p, q, r, w, s, c, df;
	int hx, ix;
	hx = __HI(x);
	ix = hx & 0x7fffffff;
	if (ix >= 0x3ff00000)
	{ /* |x| >= 1 */
		if (((ix - 0x3ff00000) | __LO(x)) == 0)
		{ /* |x|==1 */
			if (hx > 0)
			{
				return 0.0f;
			} /* acos(1) = 0  */
			else
			{
				return (float)(pi + 2.0 * pio2_lo);
			} /* acos(-1)= pi */
		}
		return (float)((x - x) / (x - x)); /* acos(|x|>1) is NaN */
	}
	if (ix < 0x3fe00000)
	{ /* |x| < 0.5 */
		if (ix <= 0x3c600000)
		{
			return (float)(pio2_hi + pio2_lo); /*if|x|<2**-57*/
		}
		z = x * x;
		p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
		q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
		r = p / q;
		return (float)(pio2_hi - (x - (pio2_lo - x * r)));
	}
	else if (hx < 0)
	{ /* x < -0.5 */
		z = (one + x) * 0.5;
		p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
		q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
		s = clib__sqrt(z);
		r = p / q;
		w = r * s - pio2_lo;
		return (float)(pi - 2.0 * (s + w));
	}
	else
	{ /* x > 0.5 */
		z = (one - x) * 0.5;
		s = clib__sqrt(z);
		df = s;
		__LO(df) = 0;
		c = (z - df * df) / (s + df);
		p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
		q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
		r = p / q;
		w = r * s + c;
		return (float)(2.0 * (df + w));
	}
}

float clib__math_get_sinus_from_rad_angle(float rad_angle)
{
	float result = 0.f;

	int degree_angle = (int)(rad_angle * 57.295779f);

	if (degree_angle < 0)
	{
		degree_angle = -(degree_angle % -360);
	}
	else
	{
		degree_angle = degree_angle % 360;
	}

	switch (degree_angle)
	{
	case 0:
		result = 0.0f;
		break;
	case 1:
		result = 0.017452f;
		break;
	case 2:
		result = 0.034899f;
		break;
	case 3:
		result = 0.052336f;
		break;
	case 4:
		result = 0.069756f;
		break;
	case 5:
		result = 0.087156f;
		break;
	case 6:
		result = 0.104528f;
		break;
	case 7:
		result = 0.121869f;
		break;
	case 8:
		result = 0.139173f;
		break;
	case 9:
		result = 0.156434f;
		break;
	case 10:
		result = 0.173648f;
		break;
	case 11:
		result = 0.190809f;
		break;
	case 12:
		result = 0.207912f;
		break;
	case 13:
		result = 0.224951f;
		break;
	case 14:
		result = 0.241922f;
		break;
	case 15:
		result = 0.258819f;
		break;
	case 16:
		result = 0.275637f;
		break;
	case 17:
		result = 0.292372f;
		break;
	case 18:
		result = 0.309017f;
		break;
	case 19:
		result = 0.325568f;
		break;
	case 20:
		result = 0.34202f;
		break;
	case 21:
		result = 0.358368f;
		break;
	case 22:
		result = 0.374607f;
		break;
	case 23:
		result = 0.390731f;
		break;
	case 24:
		result = 0.406737f;
		break;
	case 25:
		result = 0.422618f;
		break;
	case 26:
		result = 0.438371f;
		break;
	case 27:
		result = 0.45399f;
		break;
	case 28:
		result = 0.469472f;
		break;
	case 29:
		result = 0.48481f;
		break;
	case 30:
		result = 0.5f;
		break;
	case 31:
		result = 0.515038f;
		break;
	case 32:
		result = 0.529919f;
		break;
	case 33:
		result = 0.544639f;
		break;
	case 34:
		result = 0.559193f;
		break;
	case 35:
		result = 0.573576f;
		break;
	case 36:
		result = 0.587785f;
		break;
	case 37:
		result = 0.601815f;
		break;
	case 38:
		result = 0.615661f;
		break;
	case 39:
		result = 0.62932f;
		break;
	case 40:
		result = 0.642788f;
		break;
	case 41:
		result = 0.656059f;
		break;
	case 42:
		result = 0.669131f;
		break;
	case 43:
		result = 0.681998f;
		break;
	case 44:
		result = 0.694658f;
		break;
	case 45:
		result = 0.707107f;
		break;
	case 46:
		result = 0.71934f;
		break;
	case 47:
		result = 0.731354f;
		break;
	case 48:
		result = 0.743145f;
		break;
	case 49:
		result = 0.75471f;
		break;
	case 50:
		result = 0.766044f;
		break;
	case 51:
		result = 0.777146f;
		break;
	case 52:
		result = 0.788011f;
		break;
	case 53:
		result = 0.798636f;
		break;
	case 54:
		result = 0.809017f;
		break;
	case 55:
		result = 0.819152f;
		break;
	case 56:
		result = 0.829038f;
		break;
	case 57:
		result = 0.838671f;
		break;
	case 58:
		result = 0.848048f;
		break;
	case 59:
		result = 0.857167f;
		break;
	case 60:
		result = 0.866025f;
		break;
	case 61:
		result = 0.87462f;
		break;
	case 62:
		result = 0.882948f;
		break;
	case 63:
		result = 0.891007f;
		break;
	case 64:
		result = 0.898794f;
		break;
	case 65:
		result = 0.906308f;
		break;
	case 66:
		result = 0.913545f;
		break;
	case 67:
		result = 0.920505f;
		break;
	case 68:
		result = 0.927184f;
		break;
	case 69:
		result = 0.93358f;
		break;
	case 70:
		result = 0.939693f;
		break;
	case 71:
		result = 0.945519f;
		break;
	case 72:
		result = 0.951057f;
		break;
	case 73:
		result = 0.956305f;
		break;
	case 74:
		result = 0.961262f;
		break;
	case 75:
		result = 0.965926f;
		break;
	case 76:
		result = 0.970296f;
		break;
	case 77:
		result = 0.97437f;
		break;
	case 78:
		result = 0.978148f;
		break;
	case 79:
		result = 0.981627f;
		break;
	case 80:
		result = 0.984808f;
		break;
	case 81:
		result = 0.987688f;
		break;
	case 82:
		result = 0.990268f;
		break;
	case 83:
		result = 0.992546f;
		break;
	case 84:
		result = 0.994522f;
		break;
	case 85:
		result = 0.996195f;
		break;
	case 86:
		result = 0.997564f;
		break;
	case 87:
		result = 0.99863f;
		break;
	case 88:
		result = 0.999391f;
		break;
	case 89:
		result = 0.999848f;
		break;
	case 90:
		result = 1.0f;
		break;
	case 91:
		result = 0.999848f;
		break;
	case 92:
		result = 0.999391f;
		break;
	case 93:
		result = 0.99863f;
		break;
	case 94:
		result = 0.997564f;
		break;
	case 95:
		result = 0.996195f;
		break;
	case 96:
		result = 0.994522f;
		break;
	case 97:
		result = 0.992546f;
		break;
	case 98:
		result = 0.990268f;
		break;
	case 99:
		result = 0.987688f;
		break;
	case 100:
		result = 0.984808f;
		break;
	case 101:
		result = 0.981627f;
		break;
	case 102:
		result = 0.978148f;
		break;
	case 103:
		result = 0.97437f;
		break;
	case 104:
		result = 0.970296f;
		break;
	case 105:
		result = 0.965926f;
		break;
	case 106:
		result = 0.961262f;
		break;
	case 107:
		result = 0.956305f;
		break;
	case 108:
		result = 0.951057f;
		break;
	case 109:
		result = 0.945519f;
		break;
	case 110:
		result = 0.939693f;
		break;
	case 111:
		result = 0.93358f;
		break;
	case 112:
		result = 0.927184f;
		break;
	case 113:
		result = 0.920505f;
		break;
	case 114:
		result = 0.913545f;
		break;
	case 115:
		result = 0.906308f;
		break;
	case 116:
		result = 0.898794f;
		break;
	case 117:
		result = 0.891007f;
		break;
	case 118:
		result = 0.882948f;
		break;
	case 119:
		result = 0.87462f;
		break;
	case 120:
		result = 0.866025f;
		break;
	case 121:
		result = 0.857167f;
		break;
	case 122:
		result = 0.848048f;
		break;
	case 123:
		result = 0.838671f;
		break;
	case 124:
		result = 0.829038f;
		break;
	case 125:
		result = 0.819152f;
		break;
	case 126:
		result = 0.809017f;
		break;
	case 127:
		result = 0.798636f;
		break;
	case 128:
		result = 0.788011f;
		break;
	case 129:
		result = 0.777146f;
		break;
	case 130:
		result = 0.766044f;
		break;
	case 131:
		result = 0.75471f;
		break;
	case 132:
		result = 0.743145f;
		break;
	case 133:
		result = 0.731354f;
		break;
	case 134:
		result = 0.71934f;
		break;
	case 135:
		result = 0.707107f;
		break;
	case 136:
		result = 0.694658f;
		break;
	case 137:
		result = 0.681998f;
		break;
	case 138:
		result = 0.669131f;
		break;
	case 139:
		result = 0.656059f;
		break;
	case 140:
		result = 0.642788f;
		break;
	case 141:
		result = 0.62932f;
		break;
	case 142:
		result = 0.615661f;
		break;
	case 143:
		result = 0.601815f;
		break;
	case 144:
		result = 0.587785f;
		break;
	case 145:
		result = 0.573576f;
		break;
	case 146:
		result = 0.559193f;
		break;
	case 147:
		result = 0.544639f;
		break;
	case 148:
		result = 0.529919f;
		break;
	case 149:
		result = 0.515038f;
		break;
	case 150:
		result = 0.5f;
		break;
	case 151:
		result = 0.48481f;
		break;
	case 152:
		result = 0.469472f;
		break;
	case 153:
		result = 0.45399f;
		break;
	case 154:
		result = 0.438371f;
		break;
	case 155:
		result = 0.422618f;
		break;
	case 156:
		result = 0.406737f;
		break;
	case 157:
		result = 0.390731f;
		break;
	case 158:
		result = 0.374607f;
		break;
	case 159:
		result = 0.358368f;
		break;
	case 160:
		result = 0.34202f;
		break;
	case 161:
		result = 0.325568f;
		break;
	case 162:
		result = 0.309017f;
		break;
	case 163:
		result = 0.292372f;
		break;
	case 164:
		result = 0.275637f;
		break;
	case 165:
		result = 0.258819f;
		break;
	case 166:
		result = 0.241922f;
		break;
	case 167:
		result = 0.224951f;
		break;
	case 168:
		result = 0.207912f;
		break;
	case 169:
		result = 0.190809f;
		break;
	case 170:
		result = 0.173648f;
		break;
	case 171:
		result = 0.156434f;
		break;
	case 172:
		result = 0.139173f;
		break;
	case 173:
		result = 0.121869f;
		break;
	case 174:
		result = 0.104528f;
		break;
	case 175:
		result = 0.087156f;
		break;
	case 176:
		result = 0.069756f;
		break;
	case 177:
		result = 0.052336f;
		break;
	case 178:
		result = 0.034899f;
		break;
	case 179:
		result = 0.017452f;
		break;
	case 180:
		result = 0.0f;
		break;
	case 181:
		result = -0.017452f;
		break;
	case 182:
		result = -0.034899f;
		break;
	case 183:
		result = -0.052336f;
		break;
	case 184:
		result = -0.069756f;
		break;
	case 185:
		result = -0.087156f;
		break;
	case 186:
		result = -0.104528f;
		break;
	case 187:
		result = -0.121869f;
		break;
	case 188:
		result = -0.139173f;
		break;
	case 189:
		result = -0.156434f;
		break;
	case 190:
		result = -0.173648f;
		break;
	case 191:
		result = -0.190809f;
		break;
	case 192:
		result = -0.207912f;
		break;
	case 193:
		result = -0.224951f;
		break;
	case 194:
		result = -0.241922f;
		break;
	case 195:
		result = -0.258819f;
		break;
	case 196:
		result = -0.275637f;
		break;
	case 197:
		result = -0.292372f;
		break;
	case 198:
		result = -0.309017f;
		break;
	case 199:
		result = -0.325568f;
		break;
	case 200:
		result = -0.34202f;
		break;
	case 201:
		result = -0.358368f;
		break;
	case 202:
		result = -0.374607f;
		break;
	case 203:
		result = -0.390731f;
		break;
	case 204:
		result = -0.406737f;
		break;
	case 205:
		result = -0.422618f;
		break;
	case 206:
		result = -0.438371f;
		break;
	case 207:
		result = -0.45399f;
		break;
	case 208:
		result = -0.469472f;
		break;
	case 209:
		result = -0.48481f;
		break;
	case 210:
		result = -0.5f;
		break;
	case 211:
		result = -0.515038f;
		break;
	case 212:
		result = -0.529919f;
		break;
	case 213:
		result = -0.544639f;
		break;
	case 214:
		result = -0.559193f;
		break;
	case 215:
		result = -0.573576f;
		break;
	case 216:
		result = -0.587785f;
		break;
	case 217:
		result = -0.601815f;
		break;
	case 218:
		result = -0.615661f;
		break;
	case 219:
		result = -0.62932f;
		break;
	case 220:
		result = -0.642788f;
		break;
	case 221:
		result = -0.656059f;
		break;
	case 222:
		result = -0.669131f;
		break;
	case 223:
		result = -0.681998f;
		break;
	case 224:
		result = -0.694658f;
		break;
	case 225:
		result = -0.707107f;
		break;
	case 226:
		result = -0.71934f;
		break;
	case 227:
		result = -0.731354f;
		break;
	case 228:
		result = -0.743145f;
		break;
	case 229:
		result = -0.75471f;
		break;
	case 230:
		result = -0.766044f;
		break;
	case 231:
		result = -0.777146f;
		break;
	case 232:
		result = -0.788011f;
		break;
	case 233:
		result = -0.798636f;
		break;
	case 234:
		result = -0.809017f;
		break;
	case 235:
		result = -0.819152f;
		break;
	case 236:
		result = -0.829038f;
		break;
	case 237:
		result = -0.838671f;
		break;
	case 238:
		result = -0.848048f;
		break;
	case 239:
		result = -0.857167f;
		break;
	case 240:
		result = -0.866025f;
		break;
	case 241:
		result = -0.87462f;
		break;
	case 242:
		result = -0.882948f;
		break;
	case 243:
		result = -0.891007f;
		break;
	case 244:
		result = -0.898794f;
		break;
	case 245:
		result = -0.906308f;
		break;
	case 246:
		result = -0.913545f;
		break;
	case 247:
		result = -0.920505f;
		break;
	case 248:
		result = -0.927184f;
		break;
	case 249:
		result = -0.93358f;
		break;
	case 250:
		result = -0.939693f;
		break;
	case 251:
		result = -0.945519f;
		break;
	case 252:
		result = -0.951057f;
		break;
	case 253:
		result = -0.956305f;
		break;
	case 254:
		result = -0.961262f;
		break;
	case 255:
		result = -0.965926f;
		break;
	case 256:
		result = -0.970296f;
		break;
	case 257:
		result = -0.97437f;
		break;
	case 258:
		result = -0.978148f;
		break;
	case 259:
		result = -0.981627f;
		break;
	case 260:
		result = -0.984808f;
		break;
	case 261:
		result = -0.987688f;
		break;
	case 262:
		result = -0.990268f;
		break;
	case 263:
		result = -0.992546f;
		break;
	case 264:
		result = -0.994522f;
		break;
	case 265:
		result = -0.996195f;
		break;
	case 266:
		result = -0.997564f;
		break;
	case 267:
		result = -0.99863f;
		break;
	case 268:
		result = -0.999391f;
		break;
	case 269:
		result = -0.999848f;
		break;
	case 270:
		result = -1.0f;
		break;
	case 271:
		result = -0.999848f;
		break;
	case 272:
		result = -0.999391f;
		break;
	case 273:
		result = -0.99863f;
		break;
	case 274:
		result = -0.997564f;
		break;
	case 275:
		result = -0.996195f;
		break;
	case 276:
		result = -0.994522f;
		break;
	case 277:
		result = -0.992546f;
		break;
	case 278:
		result = -0.990268f;
		break;
	case 279:
		result = -0.987688f;
		break;
	case 280:
		result = -0.984808f;
		break;
	case 281:
		result = -0.981627f;
		break;
	case 282:
		result = -0.978148f;
		break;
	case 283:
		result = -0.97437f;
		break;
	case 284:
		result = -0.970296f;
		break;
	case 285:
		result = -0.965926f;
		break;
	case 286:
		result = -0.961262f;
		break;
	case 287:
		result = -0.956305f;
		break;
	case 288:
		result = -0.951057f;
		break;
	case 289:
		result = -0.945519f;
		break;
	case 290:
		result = -0.939693f;
		break;
	case 291:
		result = -0.93358f;
		break;
	case 292:
		result = -0.927184f;
		break;
	case 293:
		result = -0.920505f;
		break;
	case 294:
		result = -0.913545f;
		break;
	case 295:
		result = -0.906308f;
		break;
	case 296:
		result = -0.898794f;
		break;
	case 297:
		result = -0.891007f;
		break;
	case 298:
		result = -0.882948f;
		break;
	case 299:
		result = -0.87462f;
		break;
	case 300:
		result = -0.866025f;
		break;
	case 301:
		result = -0.857167f;
		break;
	case 302:
		result = -0.848048f;
		break;
	case 303:
		result = -0.838671f;
		break;
	case 304:
		result = -0.829038f;
		break;
	case 305:
		result = -0.819152f;
		break;
	case 306:
		result = -0.809017f;
		break;
	case 307:
		result = -0.798636f;
		break;
	case 308:
		result = -0.788011f;
		break;
	case 309:
		result = -0.777146f;
		break;
	case 310:
		result = -0.766044f;
		break;
	case 311:
		result = -0.75471f;
		break;
	case 312:
		result = -0.743145f;
		break;
	case 313:
		result = -0.731354f;
		break;
	case 314:
		result = -0.71934f;
		break;
	case 315:
		result = -0.707107f;
		break;
	case 316:
		result = -0.694658f;
		break;
	case 317:
		result = -0.681998f;
		break;
	case 318:
		result = -0.669131f;
		break;
	case 319:
		result = -0.656059f;
		break;
	case 320:
		result = -0.642788f;
		break;
	case 321:
		result = -0.62932f;
		break;
	case 322:
		result = -0.615661f;
		break;
	case 323:
		result = -0.601815f;
		break;
	case 324:
		result = -0.587785f;
		break;
	case 325:
		result = -0.573576f;
		break;
	case 326:
		result = -0.559193f;
		break;
	case 327:
		result = -0.544639f;
		break;
	case 328:
		result = -0.529919f;
		break;
	case 329:
		result = -0.515038f;
		break;
	case 330:
		result = -0.5f;
		break;
	case 331:
		result = -0.48481f;
		break;
	case 332:
		result = -0.469472f;
		break;
	case 333:
		result = -0.45399f;
		break;
	case 334:
		result = -0.438371f;
		break;
	case 335:
		result = -0.422618f;
		break;
	case 336:
		result = -0.406737f;
		break;
	case 337:
		result = -0.390731f;
		break;
	case 338:
		result = -0.374607f;
		break;
	case 339:
		result = -0.358368f;
		break;
	case 340:
		result = -0.34202f;
		break;
	case 341:
		result = -0.325568f;
		break;
	case 342:
		result = -0.309017f;
		break;
	case 343:
		result = -0.292372f;
		break;
	case 344:
		result = -0.275637f;
		break;
	case 345:
		result = -0.258819f;
		break;
	case 346:
		result = -0.241922f;
		break;
	case 347:
		result = -0.224951f;
		break;
	case 348:
		result = -0.207912f;
		break;
	case 349:
		result = -0.190809f;
		break;
	case 350:
		result = -0.173648f;
		break;
	case 351:
		result = -0.156434f;
		break;
	case 352:
		result = -0.139173f;
		break;
	case 353:
		result = -0.121869f;
		break;
	case 354:
		result = -0.104528f;
		break;
	case 355:
		result = -0.087156f;
		break;
	case 356:
		result = -0.069756f;
		break;
	case 357:
		result = -0.052336f;
		break;
	case 358:
		result = -0.034899f;
		break;
	case 359:
		result = -0.017452f;
		break;
	case 360:
		result = -0.0f;
		break;
	}

	if (rad_angle < 0.f)
	{
		result = -result;
	}

	return result;
}

//assembly som debugoval pred pouzitim, funguje, nevyhoda je ze prijma len 32 bitove hodnoty, ale postaci
uint clib__modulo(uint divident, uint divisor)
{
	uint remainder;
	_asm
	{
        mov eax, dword ptr[divident] ;toto umiestni divident do eax. Ano, je to divne ze sa pouzivaju
        mov ecx, divisor
        test ecx, ecx; kontrola ci je delitel nula
        jz devide_by_zero_error; ak je 0, skoc na label devide_by_zero
        xor edx, edx; vycisti akykolvek predchazdajuci zostaotk
        div ecx; vydel edx : eax delitelom
        mov dword ptr[remainder] ,edx
        jmp end_modulo

        devide_by_zero_error:
        mov eax, 0xFFFFFFFF
        mov dword ptr [remainder], eax

        end_modulo:
	}
	return remainder;
}

float clib__cos(float rad_angle)
{
	//cosinus function returns save value for -50 as for 50 degree angle
	//unlike sinus
	//
	//    >>  >math.sin(-3.141592653)
	//    - 5.897932257097086e-10
	//    >> > math.sin(3.141592653)
	//    5.897932257097086e-10
	//    >> > math.cos(3.141592653)
	//    - 1.0
	//    >> > math.cos(-3.141592653)
	//    - 1.0
	float result = 0.f;
	int degree_angle = (int)(rad_angle * 57.295779f);

	if (degree_angle < 0)
	{
		degree_angle = -(degree_angle % -360);
	}
	else
	{
		degree_angle = degree_angle % 360;
	}

	switch (degree_angle)
	{
	case 0:
		result = 1.0f;
		break;
	case 1:
		result = 0.999848f;
		break;
	case 2:
		result = 0.999391f;
		break;
	case 3:
		result = 0.99863f;
		break;
	case 4:
		result = 0.997564f;
		break;
	case 5:
		result = 0.996195f;
		break;
	case 6:
		result = 0.994522f;
		break;
	case 7:
		result = 0.992546f;
		break;
	case 8:
		result = 0.990268f;
		break;
	case 9:
		result = 0.987688f;
		break;
	case 10:
		result = 0.984808f;
		break;
	case 11:
		result = 0.981627f;
		break;
	case 12:
		result = 0.978148f;
		break;
	case 13:
		result = 0.97437f;
		break;
	case 14:
		result = 0.970296f;
		break;
	case 15:
		result = 0.965926f;
		break;
	case 16:
		result = 0.961262f;
		break;
	case 17:
		result = 0.956305f;
		break;
	case 18:
		result = 0.951057f;
		break;
	case 19:
		result = 0.945519f;
		break;
	case 20:
		result = 0.939693f;
		break;
	case 21:
		result = 0.93358f;
		break;
	case 22:
		result = 0.927184f;
		break;
	case 23:
		result = 0.920505f;
		break;
	case 24:
		result = 0.913545f;
		break;
	case 25:
		result = 0.906308f;
		break;
	case 26:
		result = 0.898794f;
		break;
	case 27:
		result = 0.891007f;
		break;
	case 28:
		result = 0.882948f;
		break;
	case 29:
		result = 0.87462f;
		break;
	case 30:
		result = 0.866025f;
		break;
	case 31:
		result = 0.857167f;
		break;
	case 32:
		result = 0.848048f;
		break;
	case 33:
		result = 0.838671f;
		break;
	case 34:
		result = 0.829038f;
		break;
	case 35:
		result = 0.819152f;
		break;
	case 36:
		result = 0.809017f;
		break;
	case 37:
		result = 0.798636f;
		break;
	case 38:
		result = 0.788011f;
		break;
	case 39:
		result = 0.777146f;
		break;
	case 40:
		result = 0.766044f;
		break;
	case 41:
		result = 0.75471f;
		break;
	case 42:
		result = 0.743145f;
		break;
	case 43:
		result = 0.731354f;
		break;
	case 44:
		result = 0.71934f;
		break;
	case 45:
		result = 0.707107f;
		break;
	case 46:
		result = 0.694658f;
		break;
	case 47:
		result = 0.681998f;
		break;
	case 48:
		result = 0.669131f;
		break;
	case 49:
		result = 0.656059f;
		break;
	case 50:
		result = 0.642788f;
		break;
	case 51:
		result = 0.62932f;
		break;
	case 52:
		result = 0.615661f;
		break;
	case 53:
		result = 0.601815f;
		break;
	case 54:
		result = 0.587785f;
		break;
	case 55:
		result = 0.573576f;
		break;
	case 56:
		result = 0.559193f;
		break;
	case 57:
		result = 0.544639f;
		break;
	case 58:
		result = 0.529919f;
		break;
	case 59:
		result = 0.515038f;
		break;
	case 60:
		result = 0.5f;
		break;
	case 61:
		result = 0.48481f;
		break;
	case 62:
		result = 0.469472f;
		break;
	case 63:
		result = 0.45399f;
		break;
	case 64:
		result = 0.438371f;
		break;
	case 65:
		result = 0.422618f;
		break;
	case 66:
		result = 0.406737f;
		break;
	case 67:
		result = 0.390731f;
		break;
	case 68:
		result = 0.374607f;
		break;
	case 69:
		result = 0.358368f;
		break;
	case 70:
		result = 0.34202f;
		break;
	case 71:
		result = 0.325568f;
		break;
	case 72:
		result = 0.309017f;
		break;
	case 73:
		result = 0.292372f;
		break;
	case 74:
		result = 0.275637f;
		break;
	case 75:
		result = 0.258819f;
		break;
	case 76:
		result = 0.241922f;
		break;
	case 77:
		result = 0.224951f;
		break;
	case 78:
		result = 0.207912f;
		break;
	case 79:
		result = 0.190809f;
		break;
	case 80:
		result = 0.173648f;
		break;
	case 81:
		result = 0.156434f;
		break;
	case 82:
		result = 0.139173f;
		break;
	case 83:
		result = 0.121869f;
		break;
	case 84:
		result = 0.104528f;
		break;
	case 85:
		result = 0.087156f;
		break;
	case 86:
		result = 0.069756f;
		break;
	case 87:
		result = 0.052336f;
		break;
	case 88:
		result = 0.034899f;
		break;
	case 89:
		result = 0.017452f;
		break;
	case 90:
		result = 0.0f;
		break;
	case 91:
		result = -0.017452f;
		break;
	case 92:
		result = -0.034899f;
		break;
	case 93:
		result = -0.052336f;
		break;
	case 94:
		result = -0.069756f;
		break;
	case 95:
		result = -0.087156f;
		break;
	case 96:
		result = -0.104528f;
		break;
	case 97:
		result = -0.121869f;
		break;
	case 98:
		result = -0.139173f;
		break;
	case 99:
		result = -0.156434f;
		break;
	case 100:
		result = -0.173648f;
		break;
	case 101:
		result = -0.190809f;
		break;
	case 102:
		result = -0.207912f;
		break;
	case 103:
		result = -0.224951f;
		break;
	case 104:
		result = -0.241922f;
		break;
	case 105:
		result = -0.258819f;
		break;
	case 106:
		result = -0.275637f;
		break;
	case 107:
		result = -0.292372f;
		break;
	case 108:
		result = -0.309017f;
		break;
	case 109:
		result = -0.325568f;
		break;
	case 110:
		result = -0.34202f;
		break;
	case 111:
		result = -0.358368f;
		break;
	case 112:
		result = -0.374607f;
		break;
	case 113:
		result = -0.390731f;
		break;
	case 114:
		result = -0.406737f;
		break;
	case 115:
		result = -0.422618f;
		break;
	case 116:
		result = -0.438371f;
		break;
	case 117:
		result = -0.45399f;
		break;
	case 118:
		result = -0.469472f;
		break;
	case 119:
		result = -0.48481f;
		break;
	case 120:
		result = -0.5f;
		break;
	case 121:
		result = -0.515038f;
		break;
	case 122:
		result = -0.529919f;
		break;
	case 123:
		result = -0.544639f;
		break;
	case 124:
		result = -0.559193f;
		break;
	case 125:
		result = -0.573576f;
		break;
	case 126:
		result = -0.587785f;
		break;
	case 127:
		result = -0.601815f;
		break;
	case 128:
		result = -0.615661f;
		break;
	case 129:
		result = -0.62932f;
		break;
	case 130:
		result = -0.642788f;
		break;
	case 131:
		result = -0.656059f;
		break;
	case 132:
		result = -0.669131f;
		break;
	case 133:
		result = -0.681998f;
		break;
	case 134:
		result = -0.694658f;
		break;
	case 135:
		result = -0.707107f;
		break;
	case 136:
		result = -0.71934f;
		break;
	case 137:
		result = -0.731354f;
		break;
	case 138:
		result = -0.743145f;
		break;
	case 139:
		result = -0.75471f;
		break;
	case 140:
		result = -0.766044f;
		break;
	case 141:
		result = -0.777146f;
		break;
	case 142:
		result = -0.788011f;
		break;
	case 143:
		result = -0.798636f;
		break;
	case 144:
		result = -0.809017f;
		break;
	case 145:
		result = -0.819152f;
		break;
	case 146:
		result = -0.829038f;
		break;
	case 147:
		result = -0.838671f;
		break;
	case 148:
		result = -0.848048f;
		break;
	case 149:
		result = -0.857167f;
		break;
	case 150:
		result = -0.866025f;
		break;
	case 151:
		result = -0.87462f;
		break;
	case 152:
		result = -0.882948f;
		break;
	case 153:
		result = -0.891007f;
		break;
	case 154:
		result = -0.898794f;
		break;
	case 155:
		result = -0.906308f;
		break;
	case 156:
		result = -0.913545f;
		break;
	case 157:
		result = -0.920505f;
		break;
	case 158:
		result = -0.927184f;
		break;
	case 159:
		result = -0.93358f;
		break;
	case 160:
		result = -0.939693f;
		break;
	case 161:
		result = -0.945519f;
		break;
	case 162:
		result = -0.951057f;
		break;
	case 163:
		result = -0.956305f;
		break;
	case 164:
		result = -0.961262f;
		break;
	case 165:
		result = -0.965926f;
		break;
	case 166:
		result = -0.970296f;
		break;
	case 167:
		result = -0.97437f;
		break;
	case 168:
		result = -0.978148f;
		break;
	case 169:
		result = -0.981627f;
		break;
	case 170:
		result = -0.984808f;
		break;
	case 171:
		result = -0.987688f;
		break;
	case 172:
		result = -0.990268f;
		break;
	case 173:
		result = -0.992546f;
		break;
	case 174:
		result = -0.994522f;
		break;
	case 175:
		result = -0.996195f;
		break;
	case 176:
		result = -0.997564f;
		break;
	case 177:
		result = -0.99863f;
		break;
	case 178:
		result = -0.999391f;
		break;
	case 179:
		result = -0.999848f;
		break;
	case 180:
		result = -1.0f;
		break;
	case 181:
		result = -0.999848f;
		break;
	case 182:
		result = -0.999391f;
		break;
	case 183:
		result = -0.99863f;
		break;
	case 184:
		result = -0.997564f;
		break;
	case 185:
		result = -0.996195f;
		break;
	case 186:
		result = -0.994522f;
		break;
	case 187:
		result = -0.992546f;
		break;
	case 188:
		result = -0.990268f;
		break;
	case 189:
		result = -0.987688f;
		break;
	case 190:
		result = -0.984808f;
		break;
	case 191:
		result = -0.981627f;
		break;
	case 192:
		result = -0.978148f;
		break;
	case 193:
		result = -0.97437f;
		break;
	case 194:
		result = -0.970296f;
		break;
	case 195:
		result = -0.965926f;
		break;
	case 196:
		result = -0.961262f;
		break;
	case 197:
		result = -0.956305f;
		break;
	case 198:
		result = -0.951057f;
		break;
	case 199:
		result = -0.945519f;
		break;
	case 200:
		result = -0.939693f;
		break;
	case 201:
		result = -0.93358f;
		break;
	case 202:
		result = -0.927184f;
		break;
	case 203:
		result = -0.920505f;
		break;
	case 204:
		result = -0.913545f;
		break;
	case 205:
		result = -0.906308f;
		break;
	case 206:
		result = -0.898794f;
		break;
	case 207:
		result = -0.891007f;
		break;
	case 208:
		result = -0.882948f;
		break;
	case 209:
		result = -0.87462f;
		break;
	case 210:
		result = -0.866025f;
		break;
	case 211:
		result = -0.857167f;
		break;
	case 212:
		result = -0.848048f;
		break;
	case 213:
		result = -0.838671f;
		break;
	case 214:
		result = -0.829038f;
		break;
	case 215:
		result = -0.819152f;
		break;
	case 216:
		result = -0.809017f;
		break;
	case 217:
		result = -0.798636f;
		break;
	case 218:
		result = -0.788011f;
		break;
	case 219:
		result = -0.777146f;
		break;
	case 220:
		result = -0.766044f;
		break;
	case 221:
		result = -0.75471f;
		break;
	case 222:
		result = -0.743145f;
		break;
	case 223:
		result = -0.731354f;
		break;
	case 224:
		result = -0.71934f;
		break;
	case 225:
		result = -0.707107f;
		break;
	case 226:
		result = -0.694658f;
		break;
	case 227:
		result = -0.681998f;
		break;
	case 228:
		result = -0.669131f;
		break;
	case 229:
		result = -0.656059f;
		break;
	case 230:
		result = -0.642788f;
		break;
	case 231:
		result = -0.62932f;
		break;
	case 232:
		result = -0.615661f;
		break;
	case 233:
		result = -0.601815f;
		break;
	case 234:
		result = -0.587785f;
		break;
	case 235:
		result = -0.573576f;
		break;
	case 236:
		result = -0.559193f;
		break;
	case 237:
		result = -0.544639f;
		break;
	case 238:
		result = -0.529919f;
		break;
	case 239:
		result = -0.515038f;
		break;
	case 240:
		result = -0.5f;
		break;
	case 241:
		result = -0.48481f;
		break;
	case 242:
		result = -0.469472f;
		break;
	case 243:
		result = -0.45399f;
		break;
	case 244:
		result = -0.438371f;
		break;
	case 245:
		result = -0.422618f;
		break;
	case 246:
		result = -0.406737f;
		break;
	case 247:
		result = -0.390731f;
		break;
	case 248:
		result = -0.374607f;
		break;
	case 249:
		result = -0.358368f;
		break;
	case 250:
		result = -0.34202f;
		break;
	case 251:
		result = -0.325568f;
		break;
	case 252:
		result = -0.309017f;
		break;
	case 253:
		result = -0.292372f;
		break;
	case 254:
		result = -0.275637f;
		break;
	case 255:
		result = -0.258819f;
		break;
	case 256:
		result = -0.241922f;
		break;
	case 257:
		result = -0.224951f;
		break;
	case 258:
		result = -0.207912f;
		break;
	case 259:
		result = -0.190809f;
		break;
	case 260:
		result = -0.173648f;
		break;
	case 261:
		result = -0.156434f;
		break;
	case 262:
		result = -0.139173f;
		break;
	case 263:
		result = -0.121869f;
		break;
	case 264:
		result = -0.104528f;
		break;
	case 265:
		result = -0.087156f;
		break;
	case 266:
		result = -0.069756f;
		break;
	case 267:
		result = -0.052336f;
		break;
	case 268:
		result = -0.034899f;
		break;
	case 269:
		result = -0.017452f;
		break;
	case 270:
		result = -0.0f;
		break;
	case 271:
		result = 0.017452f;
		break;
	case 272:
		result = 0.034899f;
		break;
	case 273:
		result = 0.052336f;
		break;
	case 274:
		result = 0.069756f;
		break;
	case 275:
		result = 0.087156f;
		break;
	case 276:
		result = 0.104528f;
		break;
	case 277:
		result = 0.121869f;
		break;
	case 278:
		result = 0.139173f;
		break;
	case 279:
		result = 0.156434f;
		break;
	case 280:
		result = 0.173648f;
		break;
	case 281:
		result = 0.190809f;
		break;
	case 282:
		result = 0.207912f;
		break;
	case 283:
		result = 0.224951f;
		break;
	case 284:
		result = 0.241922f;
		break;
	case 285:
		result = 0.258819f;
		break;
	case 286:
		result = 0.275637f;
		break;
	case 287:
		result = 0.292372f;
		break;
	case 288:
		result = 0.309017f;
		break;
	case 289:
		result = 0.325568f;
		break;
	case 290:
		result = 0.34202f;
		break;
	case 291:
		result = 0.358368f;
		break;
	case 292:
		result = 0.374607f;
		break;
	case 293:
		result = 0.390731f;
		break;
	case 294:
		result = 0.406737f;
		break;
	case 295:
		result = 0.422618f;
		break;
	case 296:
		result = 0.438371f;
		break;
	case 297:
		result = 0.45399f;
		break;
	case 298:
		result = 0.469472f;
		break;
	case 299:
		result = 0.48481f;
		break;
	case 300:
		result = 0.5f;
		break;
	case 301:
		result = 0.515038f;
		break;
	case 302:
		result = 0.529919f;
		break;
	case 303:
		result = 0.544639f;
		break;
	case 304:
		result = 0.559193f;
		break;
	case 305:
		result = 0.573576f;
		break;
	case 306:
		result = 0.587785f;
		break;
	case 307:
		result = 0.601815f;
		break;
	case 308:
		result = 0.615661f;
		break;
	case 309:
		result = 0.62932f;
		break;
	case 310:
		result = 0.642788f;
		break;
	case 311:
		result = 0.656059f;
		break;
	case 312:
		result = 0.669131f;
		break;
	case 313:
		result = 0.681998f;
		break;
	case 314:
		result = 0.694658f;
		break;
	case 315:
		result = 0.707107f;
		break;
	case 316:
		result = 0.71934f;
		break;
	case 317:
		result = 0.731354f;
		break;
	case 318:
		result = 0.743145f;
		break;
	case 319:
		result = 0.75471f;
		break;
	case 320:
		result = 0.766044f;
		break;
	case 321:
		result = 0.777146f;
		break;
	case 322:
		result = 0.788011f;
		break;
	case 323:
		result = 0.798636f;
		break;
	case 324:
		result = 0.809017f;
		break;
	case 325:
		result = 0.819152f;
		break;
	case 326:
		result = 0.829038f;
		break;
	case 327:
		result = 0.838671f;
		break;
	case 328:
		result = 0.848048f;
		break;
	case 329:
		result = 0.857167f;
		break;
	case 330:
		result = 0.866025f;
		break;
	case 331:
		result = 0.87462f;
		break;
	case 332:
		result = 0.882948f;
		break;
	case 333:
		result = 0.891007f;
		break;
	case 334:
		result = 0.898794f;
		break;
	case 335:
		result = 0.906308f;
		break;
	case 336:
		result = 0.913545f;
		break;
	case 337:
		result = 0.920505f;
		break;
	case 338:
		result = 0.927184f;
		break;
	case 339:
		result = 0.93358f;
		break;
	case 340:
		result = 0.939693f;
		break;
	case 341:
		result = 0.945519f;
		break;
	case 342:
		result = 0.951057f;
		break;
	case 343:
		result = 0.956305f;
		break;
	case 344:
		result = 0.961262f;
		break;
	case 345:
		result = 0.965926f;
		break;
	case 346:
		result = 0.970296f;
		break;
	case 347:
		result = 0.97437f;
		break;
	case 348:
		result = 0.978148f;
		break;
	case 349:
		result = 0.981627f;
		break;
	case 350:
		result = 0.984808f;
		break;
	case 351:
		result = 0.987688f;
		break;
	case 352:
		result = 0.990268f;
		break;
	case 353:
		result = 0.992546f;
		break;
	case 354:
		result = 0.994522f;
		break;
	case 355:
		result = 0.996195f;
		break;
	case 356:
		result = 0.997564f;
		break;
	case 357:
		result = 0.99863f;
		break;
	case 358:
		result = 0.999391f;
		break;
	case 359:
		result = 0.999848f;
		break;
	case 360:
		result = 1.0f;
		break;
	}

	return result;
}

float clib__fabs(float value)
{
	if (value < 0.0f)
	{
		value = -value;
		return value;
	}
	return value;
}

boole clib__is_string_equal_to_another_string(cstring str1, cstring str2)
{
	//isnt there like a.. assemblt instruction for this? that would make it a lot faster
	//like real strcmp, we return FALSE if strings are EQUAL true if not equal
	boole result = TRUE;

	if (str1 == NULL || str2 == NULL)
	{
		result = FALSE;
		return result;
	}

	int a = (int)clib__string_length(str1);
	int b = (int)clib__string_length(str2);

	if (a != b)
	{
		result = FALSE;
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
				result = FALSE;
				break;
			}
		}
	}
	return result;
}

boole clib__is_wide_string_equal_to_another_wide_string(wchar_t *str1, wchar_t *str2)
{
	boole result = TRUE;

	if (str1 == NULL_POINTER || str2 == NULL_POINTER)
	{
		result = FALSE;
		return result;
	}

	int a = (int)clib__widestring_length(str1);
	int b = (int)clib__widestring_length(str2);

	//najprv sa skontroluje ci su rovnake dlzky stringu, dava to viac zmysel?
	//ani nie

	if (a != b)
	{
		result = FALSE;
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
				result = FALSE;
				break;
			}
		}
	}
	return result;

	// while (*str1 != 0 && (*str1 == *str2))
	// {
	//     str1++;
	//     str2++;
	// }
	//
	//
	// return (*str1 == *str2);
}

static wchar_t _clib_internal__convert_wide_char_to_lower(wchar_t wc)
{
	if (wc >= L'A' && wc <= L'Z')
	{
		return wc + (L'a' - L'A');
	}
	return wc;
}

//ok, detours k tomuto potreboval fastcall__ stdcall padal.

boole clib__is_wstring_equal_to_another_wstring_ignore_case(wchar_t *str1, wchar_t *str2)
{
	while (*str1 && (_clib_internal__convert_wide_char_to_lower(*str1) == _clib_internal__convert_wide_char_to_lower(*str2)))
	{
		++str1;
		++str2;
	}

	return (boole)(_clib_internal__convert_wide_char_to_lower(*str1) == _clib_internal__convert_wide_char_to_lower(*str2));
}

boole clib__is_wstring_equal_to_another_wstring(wchar_t *str1, wchar_t *str2)
{
	if (str1 == NULL_POINTER || str2 == NULL_POINTER)
	{
		return FALSE;
	}

	while (*str1 && (*str1 == *str2))
	{
		++str1;
		++str2;
	}

	return (boole)(*str1 == *str2);
}

boole clib__is_address_pointing_to_string(char *string)
{
	//ci je adresa platna na citanie  skontrolovat inde cez prislusnu api prieliehajucu operacnemu systemu
	//ASCII string
	nuint curr_string_length = 0;

	//string length limit, 1000

	boole status = memory_manager__is_memory_block_valid_for_reading((nuint)string, 1000);

	if (status == FALSE)
	{
#ifdef DEBUG_ACTIVE
		// console_write("%s", "clib__is_address_pointing_to_string  false \n");
#endif
		return 0;
	}

	nuint i;
	for (i = 0; i < 1000; i++)
	{
		char single_char = *(string + i);
		if (single_char == 0)
		{
			if (i == 0)
			{
				return FALSE;
			}
			else
			{
				return TRUE;
			}
		}
		curr_string_length++;
	}
	return FALSE;
}

boole clib__is_little_endian()
{
	unsigned num = 0xABCD;
	return *((ubyte *)&num) == 0xCD;
}

void clib__print_block_of_memory(void *base, nint length)
{
#ifdef DEBUG_CLIB_ACTIVE
	nuint base_address = (nuint)base;
	int x = 0;
	for (x = 0; x < length; x++)
	{
		base_address += 1;
		ubyte a = *(ubyte *)base_address;
		console_write("%s %p %s %X %s", "base_address ", base_address, " -> ", a, "\n");
	}
#endif
}

void clib__print_bits_of_byte(ubyte value_to_print)
{
	ubyte help_byte = 1;
	signed char i;
	for (i = 7; i >= 0; i--)
	{
		ubyte lol = (help_byte << i) & value_to_print;

		if (lol)
		{
			//printf("1");
		}
		else
		{
			//printf("0");
		}
	}
}

void clib__print_bits_of_uint64(nuint value_to_print)
{
	signed char i;

	for (i = 7; i >= 0; i--)
	{
		ubyte byte_to_print = ((ubyte *)&value_to_print)[i];

		//printf("  ");

		clib__print_bits_of_byte(byte_to_print);
	}
}

uint clib__hex2int(char *hex)
{
	uint val = 0;
	while (*hex)
	{
		char byte = *hex++;
		if (byte >= '0' && byte <= '9')
		{
			byte = byte - '0';
		}
		else if (byte >= 'a' && byte <= 'f')
		{
			byte = byte - 'a' + 10;
		}
		else if (byte >= 'A' && byte <= 'F')
		{
			byte = byte - 'A' + 10;
		}
		val = (val << 4) | (byte & 0xF);
	}
	return val;
}

ubyte clib__remove_flag(ubyte arg_value1, ubyte flag)
{
	ubyte v1 = 255;
	ubyte result = (((arg_value1 & flag) ^ v1) & arg_value1);
	return result;
}

//byte clib__set_bit_off(byte arg_value1, byte bit_position)
//{
//    bit_position = 7 - bit_position - 1;
//    byte v1 = 255;
//    byte single_bit = 1; //this bit is at position 8
//    //int single_bit1 = single_bit << bit_position;
//
//    single_bit = single_bit << bit_position;
//    byte result = ((single_bit & arg_value1) ^ v1) & arg_value1;
//    return result;
//}

boole clib__is_bit_active_in_uint64(uint64 value, int bit_position)
{
	boole result = FALSE;
	int shift = -1;
	shift += bit_position;

	nuint help_value = 1;

	help_value = help_value << shift;

	//printf("%s %d %s ", "shift: ", shift , "\n");
	//printf("%s %d %s ", "bit_position: ", bit_position, "\n");

	//printf("help_value -> ");
	//clib__print_bits_of_uint64(help_value);
	//printf(" \n");

	//printf("value    ->   ");
	//clib__print_bits_of_uint64(value);
	//printf(" \n");

	//printf("help_value");
	nuint bitwise_operation_result = value & help_value;

	//printf("resl_value -> ");
	//clib__print_bits_of_uint64(bitwise_operation_result);
	//printf(" \n");

	if (bitwise_operation_result > 0)
	{
		//printf("%s %d %s", "bit position", shift, "\n");
		result = TRUE;
	}
	return result;
}

boole clib__is_bit_active_in_uint32(uint value, int bit_index)
{
	return (value & (1 << bit_index)) != 0;
}

nuint clib__string_length(cstring arg_string)
{
	//ASCII string
	nuint curr_string_length = 0;

	//string length limit, 1000

	nuint i;
	for (i = 0; i < 1000; i++)
	{
		char single_char = *(arg_string + i);
		if (single_char == 0)
		{
			break;
		}
		curr_string_length++;
	}
	return curr_string_length;
}

nuint clib__widestring_length(wchar_t *arg_string)
{
	//ASCII string
	nuint curr_string_length = 0;

	//string length limit, 1000

	nuint i;
	for (i = 0; i < 1000; i++)
	{
		wchar_t single_char = *(arg_string + i);
		if (single_char == 0)
		{
			break;
		}
		curr_string_length++;
	}
	return curr_string_length;
}

nuint clib__find_address_in_module_address_space_use_pattern(HMODULE moduleHandle, cstring pattern, nuint offset)
{
	MODULEINFO moduleInfo;
	memset((void *)&moduleInfo, 0, sizeof(MODULEINFO)); //bezpecnost pri praci s pamatou

	if (moduleHandle && meh->winapi->f.psapi.GetModuleInformation(meh->winapi->winapi__current_process_handle, moduleHandle, &moduleInfo, sizeof(moduleInfo)))
	{
		cstring begin = moduleInfo.lpBaseOfDll;
		cstring end = begin + moduleInfo.SizeOfImage;

		for (cstring c = begin; c != end; c++)
		{
			boole matched = TRUE;

			for (cstring patternIt = pattern, it = c; *patternIt; patternIt++, it++)
			{
				if (*patternIt != '?' && *it != *patternIt)
				{
					matched = FALSE;
					break;
				}
			}
			if (matched)
			{
				return (nuint)(c + offset);
			}
		}
	}
	return 0;
}

nuint clib__find_address_in_module_address_space_use_pattern_my_version(HMODULE moduleHandle, cstring pattern, nuint offset)
{
	MODULEINFO moduleInfo;
	memset((void *)&moduleInfo, 0, sizeof(MODULEINFO)); //bezpecnost pri praci s pamatou

	//skontroluje sa ci module handle naozaj existuje
	if (!moduleHandle)
	{
		return 0;
	}

	if (!(((winapi_t *)meh->winapi)->f.psapi.GetModuleInformation(((winapi_t *)meh->winapi)->winapi__current_process_handle, moduleHandle, &moduleInfo, sizeof(moduleInfo))))
	{
		return 0;
	}

	nuint start_address = (nuint)(moduleInfo.lpBaseOfDll);
	nuint number_of_bytes_to_scan = start_address + moduleInfo.SizeOfImage;

	// console_write("%s %d %s", "|clib__search_for_pattern_in_module_address_space| number_of_bytes_to_scan ", number_of_bytes_to_scan, "\n");

	nuint result = 0;
	int whitespace_count = 0;
	size_t i;

	//ziska sa pocet mezdier v skanovanom stringu. Tak sa zisti kolko bytov sa hlada
	for (i = 0; i < clib__string_length(pattern); i++)
	{
		const char *whitespace = " ";
		if (pattern[i] == whitespace[0])
		{
			whitespace_count++;
		}
	}

	uint pattern_found = 0;
	int pattern_length = (whitespace_count + 1);
	char *memory_block = (char *)start_address;

	nuint y;

	for (y = 0; y < number_of_bytes_to_scan - pattern_length; y++)
	{
		if (!pattern_found)
		{
			int z;
			for (z = 0; z < pattern_length; z++)
			{
				ubyte scanned_byte = (ubyte)memory_block[y + (nuint)z];
				char byte_in_hex_string[3] = { 0, 0, 0 };
				int source_offset = z * 2 + z;
				clib__custom_memory_copy((ubyte *)pattern, (ubyte *)&byte_in_hex_string[0], source_offset, 0, 2);
				if (clib__is_string_equal_to_another_string(byte_in_hex_string, "??") == TRUE)
				{
					continue;
				}
				ubyte read_byte = (ubyte)clib__hexstring_to_uint64((char *)&byte_in_hex_string[0]);
				if (scanned_byte != read_byte)
				{
					break;
				}
				if (z + 1 == pattern_length)
				{
					pattern_found = 1;
					result = start_address + y;
				}
			}
		}
		else
		{
			break;
		}
	}
	return result + offset;
}

uint clib__find_offset_in_buffer_use_pattern(uint start_offset, ubyte *buffer, uint size_of_address_space_to_search_in, ubyte *bytes_to_find, uint buffer_length)
{
	uint result = 0;
	nuint i;
	uint ii;
	for (i = 0; i < size_of_address_space_to_search_in; i++)
	{
		for (ii = 0; ii < buffer_length; ii++)
		{
			ubyte read_byte = *((ubyte *)(buffer + start_offset + i + ii));

			if (read_byte != bytes_to_find[ii])
			{
#ifdef USING_NO_DEFAULT_LIB

				//console_write("%s %d %s %d %s", "read_byte ", read_byte, "!=", bytes_to_find[ii],"\n");
#endif

				break;
			}

			else if (read_byte == bytes_to_find[ii])
			{
#ifndef USING_NO_DEFAULT_LIB
				//printf("%s %d %s %d %s", "read_byte ", read_byte, " EQUALS ", bytes_to_find[ii], "\n");
#endif
			}

			if (ii + 1 == buffer_length)
			{
				result = start_offset + (uint)i + ii;
				return result;
			}
		}
	}
	return result;
}

nuint clib__find_address_in_buffer_use_pattern(uint start_offset, ubyte *buffer, uint size_of_address_space_to_search_in, ubyte *bytes_to_find, uint buffer_length)
{
	nuint result = 0;
	nuint i;
	uint ii;
	for (i = 0; i < size_of_address_space_to_search_in; i++)
	{
		for (ii = 0; ii < buffer_length; ii++)
		{
			ubyte read_byte = *((ubyte *)(buffer + start_offset + i + ii));

			if (read_byte != bytes_to_find[ii])
			{
#ifdef USING_NO_DEFAULT_LIB

				// console_write("%s %d %s %d %s", "read_byte ", read_byte, "!=", bytes_to_find[ii], "\n");
#endif
				break;
			}

			else if (read_byte == bytes_to_find[ii])
			{
#ifndef USING_NO_DEFAULT_LIB
				//printf("%s %d %s %d %s", "read_byte ", read_byte, " EQUALS ", bytes_to_find[ii], "\n");
#endif
			}

			if (ii + 1 == buffer_length)
			{
				result = (nuint)(buffer + start_offset + i + ii + 1);
				return result;
			}
		}
	}
	return result;
}

nuint clib__find_address_use_pattern_and_use_mask(cstring pattern, cstring mask, nuint start, nuint end, nuint offset)
{
	int patternLength = clib__string_length(pattern);
	boole found = FALSE;

	nuint i = 0;

	for (i = start; i < end - patternLength; i++)
	{
		found = TRUE;
		for (unsigned int idx = 0; idx < patternLength; idx++)
		{
			if (mask[idx] == 'x' && pattern[idx] != *(char *)(i + idx))
			{
				found = FALSE;
				break;
			}
		}
		if (found)
		{
			return i + offset;
		}
	}
	return 0;
}

nuint clib__find_address_use_pattern(cstring pattern, nuint start, nuint end, nuint offset)
{
	nuint i = 0;

	if (start > end)
	{
		nuint reverse = end;
		end = start;
		start = reverse;
	}

	nuint patternLength = clib__string_length(pattern);

	boole found = FALSE;

	for (i = start; i < end - patternLength; i++)
	{
		found = TRUE;

		for (nuint idx = 0; idx < patternLength; idx++)
		{
			if (pattern[idx] != *(char *)(i + idx))
			{
				found = FALSE;
				break;
			}
		}

		if (found)
		{
			return i + offset;
		}
	}
	return 0;
}

nuint clib__find_push_instruction(nuint start, nuint end, cstring Message)
{
	char bPushAddrPattern[] = { 0x68, 0x00, 0x00, 0x00, 0x00, 0x00 };
	nuint Address = clib__find_address_use_pattern(Message, start, end, 0);
	*(nuint *)&bPushAddrPattern[1] = Address;
	Address = clib__find_address_use_pattern((char *)bPushAddrPattern, start, end, 0);
	return Address;
}

void clib__null_memory(void *source, nuint length)
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

void clib__copy_memory(void *source, void *destination, nint length, nint max_allowed_length)
{
	nint already_copied_bytes = 0;
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

//bool_ clib__create_file_from_buffer(char* new_file_name, byte* _buffer, size_t _length)
//{
//    //this is wrong
//    FILE* pFile;
//
//    errno_t status = 0;
//
//    status = winapi->f.msvcrt.fopen_s(&pFile, new_file_name, "wb");
//
//    if (status == 0)
//    {
//#ifdef USING_NO_DEFAULT_LIB
//        console_write("%s%s%s", "[!] fopen(wb) on file path ", new_file_name, " failed \n");
//#endif
//        return FALSE;
//    }
//
//
//#ifdef USING_NO_DEFAULT_LIB
//    console_write("%s%s%s", "clib__create_file_from_buffer ", new_file_name, " success \n");
//#endif
//
//    return TRUE;
//}

boole clib__read_file(cstring filename, ubyte **out_buffer, uint64 *output_file_buffer_length)
{
	char *buffer;
	size_t file_size;
	HANDLE file_handle;

	errno_t status = 0;
	DWORD number_of_bytes_read;

	DBG_CLIB console__write("%s %s %s", "clib__read_file trying to read file ", filename, "\n");

	file_handle = meh->winapi->f.kernel32.CreateFileA(filename, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (file_handle == INVALID_HANDLE_VALUE)
	{
		DBG_CLIB console__write("%s", "clib__read_file INVALID_HANDLE_VALUE, check if file really exists \n");
		return FALSE;
	}

	DWORD file_size1 = meh->winapi->f.kernel32.GetFileSize(file_handle, NULL);
	if (file_size1 == INVALID_FILE_SIZE)
	{
		DBG_CLIB console__write("%s", "clib__read_file INVALID_FILE_SIZE \n");

		meh->winapi->f.kernel32.CloseHandle(file_handle);
		return FALSE;
	}

	nuint size_to_allocate = file_size1 + 1; //null terminator

	buffer = (char *)memory_manager__alloc(size_to_allocate); // null terminator
	if (buffer == NULL_POINTER)
	{
		DBG_CLIB console__write("[!] failed allocating space for buffer");
		meh->winapi->f.kernel32.CloseHandle(file_handle);
		return FALSE;
	}

	memset(buffer, 0, size_to_allocate);

	status = meh->winapi->f.kernel32.ReadFile(file_handle, buffer, file_size1, &number_of_bytes_read, NULL);
	if (status == FALSE || number_of_bytes_read != file_size1)
	{
		DBG_CLIB console__write("%s", "clib__read_file ReadFile fail \n");
		meh->winapi->f.kernel32.CloseHandle(file_handle);
		return FALSE;
	}
	// obtain file size:

	*output_file_buffer_length = file_size1;
	*out_buffer = (ubyte *)buffer;
	DBG_CLIB console__write("[i] file read successfully into memory buffer \n");
	meh->winapi->f.kernel32.CloseHandle(file_handle);

	return TRUE;
}

void clib__convert_hexstring_to_longlong(nuint *output, char *hexstring)
{
	char buf[10] = { 0 };
	char c;
	while ((c = *hexstring++))
	{
		buf[0] = c;
		*output++ = clib__strtol(buf, NULL, 16);
	}
}

uint64 clib__hexstring_to_uint64(char *hex_string)
{
	uint64 parsed_number = MAXULONG64;

	// check if chars are valid

	if (clib__string_length(hex_string) == 0)
	{
		return 0;
	}

	char valid_chars[] = "0123456789ABCDEFabcdef";

	int i;
	int y;

	int string_length1 = (int)clib__string_length(hex_string);
	int string_length2 = (int)clib__string_length(valid_chars);

	//console_write("%s %d %s", "clib__hexstring_to_uint64 input length ", string_length1, "\n");
	//console_write("%s %d %s", "clib__hexstring_to_uint64 input length ", string_length2, "\n");

	//try to find char that is not valid, within the input string.
	//if one is found, return fail

	for (i = 0; i < string_length1; i++)
	{
		for (y = 0; y < string_length2; y++)
		{
			char char1 = *(hex_string + i);
			char char2 = *(valid_chars + y);

			if (char1 == char2)
			{
				//console_write("%s %c %s %c %s", "[s] char1 ", char1, "char2", char2, "\n");
				break;
			}
			else
			{
				if ((y + 1) == string_length2)
				{
					// console_write("%s", "clib__hexstring_to_uint64 string not valid \n");
					return 0;
				}
			}
		}
	}

	//
	//pasted from github
	//brilliant solution
	//

	parsed_number = 0;

	for (i = 0; hex_string[i] != '\0'; i++)
	{
		if ((hex_string[i] == '0' && hex_string[i + 1] == 'x') || hex_string[i] == 'x')
		{
			continue;
		}

		int value = 0;

		if (hex_string[i] >= '0' && hex_string[i] <= '9')
		{
			value = hex_string[i] - '0';
		}
		else if (hex_string[i] >= 'a' && hex_string[i] <= 'z')
		{
			value = (hex_string[i] - 'a') + 10;
		}
		else if (hex_string[i] >= 'A' && hex_string[i] <= 'Z')
		{
			value = (hex_string[i] - 'A') + 10;
		}

		parsed_number = parsed_number * 16 + (nuint)value; // multiply by 16 for correct digit shifting because we're iterating left to right.
	}

	return parsed_number;
}

boole clib__memory_compare(const void *ptr1, const void *ptr2, uint num)
{
	register const unsigned char *s1 = (const unsigned char *)ptr1;
	register const unsigned char *s2 = (const unsigned char *)ptr2;

	while (num-- > 0)
	{
		if (*s1++ != *s2++)
		{
			return s1[-1] < s2[-1] ? -1 : 1;
		}
	}
	return 0;
}

//interne pouzitie
static int _clib_internal__compare(const char *s1, const char *s2)
{
	while (*s1 && *s2)
	{
		if (*s1 != *s2)
		{
			return 0;
		}
		s1++;
		s2++;
	}
	return *s2 == 0;
}

//zisti ci sa v stringu nachazda iny string
const char *clib__strstr(const char *s1, const char *s2)
{
	while (*s1)
	{
		if ((*s1 == *s2) && _clib_internal__compare(s1, s2))
		{
			return s1;
		}
		s1++;
	}
	return NULL;
}

//navrati 0 ak sa stringy rovnaju.
int clib__strcmp(const char *str1, const char *str2)
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

static int is_character_space(unsigned char c)
{
	return (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == ' ' ? 1 : 0);
}

long clib__strtol(const char *nptr, char **endptr, int base)
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
	s = nptr;
	do
	{
		c = *s++;
	} while (is_character_space((unsigned char)c));

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
		*endptr = (char *)(any ? s - 1 : nptr);
	}
	return (acc);
}

int clib__strncmp(const char *str1, const char *str2, int number_of_bytes_to_compare)
{
	int current_byte = 0;
	while (current_byte < number_of_bytes_to_compare) //pokial obidva stringy nenarazia na null terminator
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

//navrati 0 ak sa stringy rovnaju, avsak nezalezi na tom ci su velke alebo male pismenko
int clib__stricmp(const char *str1, const char *str2)
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

//---------------------------------------------------------------------------
// IEEE 754 double MSW masks
const uint _f64_sig = 0x80000000; // sign
const uint _f64_exp = 0x7FF00000; // exponent
const uint _f64_exp_sig = 0x40000000; // exponent sign
const uint _f64_exp_bia = 0x3FF00000; // exponent bias
const uint _f64_exp_lsb = 0x00100000; // exponent LSB
const uint _f64_exp_pos = 20; // exponent LSB bit position
const uint _f64_man = 0x000FFFFF; // mantisa
const uint _f64_man_msb = 0x00080000; // mantisa MSB
const uint _f64_man_bits = 52; // mantisa bits
// IEEE 754 single masks
const uint _f32_sig = 0x80000000; // sign
const uint _f32_exp = 0x7F800000; // exponent
const uint _f32_exp_sig = 0x40000000; // exponent sign
const uint _f32_exp_bia = 0x3F800000; // exponent bias
const uint _f32_exp_lsb = 0x00800000; // exponent LSB
const uint _f32_exp_pos = 23; // exponent LSB bit position
const uint _f32_man = 0x007FFFFF; // mantisa
const uint _f32_man_msb = 0x00400000; // mantisa MSB
const uint _f32_man_bits = 23; // mantisa bits
//---------------------------------------------------------------------------

double clib__floor64(double x)
{
	const int h = 1; // may be platform dependent MSB/LSB order
	const int l = 0;
	union _f64 // semi result
	{
		double f; // 64bit floating point
		uint u[2]; // 2x32 bit uint
	} y;
	uint m, a;
	int sig, exp, sh;
	y.f = x;
	// extract sign
	sig = y.u[h] & _f64_sig;
	// extract exponent
	exp = ((y.u[h] & _f64_exp) >> _f64_exp_pos) - (_f64_exp_bia >> _f64_exp_pos);
	// floor bit shift
	sh = _f64_man_bits - exp;
	a = 0;
	if (exp < 0)
	{
		a = y.u[l] | (y.u[h] & _f64_man);
		if (sig)
		{
			return -1.0;
		}
		return 0.0;
	}
	// LSW
	if (sh > 0)
	{
		if (sh < 32)
		{
			m = (0xFFFFFFFF >> sh) << sh;
		}
		else
		{
			m = 0;
		}
		a = y.u[l] & (m ^ 0xFFFFFFFF);
		y.u[l] &= m;
	}
	// MSW
	sh -= 32;
	if (sh > 0)
	{
		if (sh < _f64_exp_pos)
		{
			m = (0xFFFFFFFF >> sh) << sh;
		}
		else
		{
			m = _f64_sig | _f64_exp;
		}
		a |= y.u[h] & (m ^ 0xFFFFFFFF);
		y.u[h] &= m;
	}
	if ((sig) && (a))
	{
		y.f--;
	}
	return y.f;
}
//---------------------------------------------------------------------------

float clib__floor32(float x)
{
	union // semi result
	{
		float f; // 32bit floating point
		uint u; // 32 bit uint
	} y;
	uint m, a;
	int sig, exp, sh;
	y.f = x;
	// extract sign
	sig = y.u & _f32_sig;
	// extract exponent
	exp = ((y.u & _f32_exp) >> _f32_exp_pos) - (_f32_exp_bia >> _f32_exp_pos);
	// floor bit shift
	sh = _f32_man_bits - exp;
	a = 0;
	if (exp < 0)
	{
		a = y.u & _f32_man;
		if (sig)
		{
			return -1.0;
		}
		return 0.0;
	}
	if (sh > 0)
	{
		if (sh < _f32_exp_pos)
		{
			m = (0xFFFFFFFF >> sh) << sh;
		}
		else
		{
			m = _f32_sig | _f32_exp;
		}
		a |= y.u & (m ^ 0xFFFFFFFF);
		y.u &= m;
	}
	if ((sig) && (a))
	{
		y.f--;
	}
	return y.f;
}
//---------------------------------------------------------------------------

float clib__ceil(float f)
{
	unsigned input;
	memcpy(&input, &f, 4);
	int exponent = ((input >> 23) & 255) - 127;
	if (exponent < 0)
	{
		return (f > 0);
	}
	// small numbers get rounded to 0 or 1, depending on their sign

	int fractional_bits = 23 - exponent;
	if (fractional_bits <= 0)
	{
		return f;
	}
	// numbers without fractional bits are mapped to themselves

	unsigned integral_mask = 0xffffffff << fractional_bits;
	unsigned output = input & integral_mask;
	// round the number down by masking out the fractional bits

	memcpy(&f, &output, 4);
	if (f > 0 && output != input)
	{
		++f;
	}
	// positive numbers need to be rounded up, not down

	return f;
}

int clib__truncate(float x)
{
	return (int)(x < 0 ? clib__ceil(x) : clib__floor32(x));
}

//od delenca (x),  x, sa odcita cast ktora sa da rozdelit  (float)clib__truncate(x / y) * y;, to je zvysok
float clib__fmod(float x, float y)
{
	return x - (float)clib__truncate(x / y) * y;
}

//void swap(void* v1, void* v2, int size)
//{
//    // buffer is array of characters which will
//    // store element byte by byte
//    char buffer[size];
//
//    // memcpy will copy the contents from starting
//    // address of v1 to length of size in buffer
//    // byte by byte.
//    memcpy(buffer, v1, size);
//    memcpy(v1, v2, size);
//    memcpy(v2, buffer, size);
//}
//
//// v is an array of elements to sort.
//// size is the number of elements in array
//// left and right is start and end of array
////(*comp)(void*, void*) is a pointer to a function
//// which accepts two void* as its parameter
//void clib__qsort(void* v, int size, int left, int right, int (*comp)(void*, void*))
//{
//    void* vt, * v3;
//    int i, last, mid = (left + right) / 2;
//    if (left >= right)
//    {
//        return;
//    }
//
//    // casting void* to char* so that operations
//    // can be done.
//    void* vl = (char*)(v + (left * size));
//    void* vr = (char*)(v + (mid * size));
//    swap(vl, vr, size);
//    last = left;
//    for (i = left + 1; i <= right; i++)
//    {
//
//        // vl and vt will have the starting address
//        // of the elements which will be passed to
//        // comp function.
//        vt = (char*)(v + (i * size));
//        if ((*comp)(vl, vt) > 0)
//        {
//            ++last;
//            v3 = (char*)(v + (last * size));
//            swap(vt, v3, size);
//        }
//    }
//    v3 = (char*)(v + (last * size));
//    swap(vl, v3, size);
//    _qsort(v, size, left, last - 1, comp);
//    _qsort(v, size, last + 1, right, comp);
//}

static double expo(double n)
{
	int a = 0;
	int b = n > 0;
	double c = 1.0;
	double d = 1.0;
	double e = 1.0;

	for (b || (n = -n); e + .00001 < (e += (d *= n) / (c *= ++a));)
		;
	// approximately 15 iterations
	return b ? e : 1 / e;
}

static double native_log_computation(const double n)
{
	// Basic logarithm computation.
	static const double euler = 2.7182818284590452354;
	unsigned a = 0, d;
	double b, c, e, f;
	if (n > 0)
	{
		for (c = n < 1 ? 1 / n : n; (c /= euler) > 1; ++a)
			;
		c = 1 / (c * euler - 1), c = c + c + 1, f = c * c, b = 0;
		for (d = 1, c /= 2; e = b, b += 1 / (d * c), b - e /* > 0.0000001 */;)
		{
			d += 2, c *= f;
		}
	}
	else
	{
		b = (n == 0) / 0.;
	}
	return n < 1 ? -(a + b) : a + b;
}

double clib__ln(const double n)
{
	//  Returns the natural logarithm (base e) of N.
	return native_log_computation(n);
}

double clib__log(const double n, const double base)
{
	//  Returns the logarithm (base b) of N.
	return native_log_computation(n) / native_log_computation(base);
}

nuint clib__find_reference(nuint start, nuint end, nuint address)
{
	//ako funguje tento shit?
	char szPattern[] = { 0x68, 0x00, 0x00, 0x00, 0x00, 0x00 };
	*(nuint *)&szPattern[1] = address;
	return clib__find_address_use_pattern(szPattern, start, end, 0);
}

//skontroluje ci sa adresa nachazda v ramci adresneho priestoru
boole clib__is_address_within_bounds(nuint address_to_check, nuint from, nuint to)
{
	boole status = (boole)((address_to_check >= from) && (address_to_check < to));
	return status;
}

#define TABLE_SIZE 512 // Number of entries in the table
#define PI_OVER_2 (MATH_PI / 2)

// Factorial function for the Taylor series
unsigned long long factorial(int n)
{
	unsigned long long result = 1;
	for (int i = 1; i <= n; i++)
	{
		result *= i;
	}
	return result;
}

#define SINE_TABLE_SIZE 1024
static float sine_table[SINE_TABLE_SIZE];

#define PI 3.14159265358979323846

// Taylor series for sin(x) around 0, good for small angles
float taylor_sinf(float x)
{
	// Reduce x to [-π, π] for better convergence
	while (x > PI)
	{
		x -= 2.0 * PI;
	}
	while (x < -PI)
	{
		x += 2.0 * PI;
	}

	// Taylor series: sin(x) ≈ x - x^3/3! + x^5/5! - x^7/7! + ...
	float x2 = x * x;
	float term = x; // First term: x
	float result = term;
	// Add terms up to 7th order for decent precision
	term *= -x2 / (2.0 * 3.0); // -x^3/3!
	result += term;
	term *= -x2 / (4.0 * 5.0); // x^5/5!
	result += term;
	term *= -x2 / (6.0 * 7.0); // -x^7/7!
	result += term;
	return result;
}

void init_sine_table(void)
{
	for (int i = 0; i < SINE_TABLE_SIZE; i++)
	{
		float angle = (float)i * 2.0 * PI / SINE_TABLE_SIZE;
		sine_table[i] = taylor_sinf(angle);
	}
}

boole is_sine_table_initialized = FALSE;

float clib__sinf(float x)
{
	if (is_sine_table_initialized == FALSE)
	{
		init_sine_table();
		is_sine_table_initialized = TRUE;
	}

	// Normalize angle to [0, 2π]
	x = clib__fmod(x, 2.0 * PI);
	if (x < 0)
	{
		x += 2.0 * PI;
	}

	// Scale to table index
	float index = x * (SINE_TABLE_SIZE / (2.0 * PI));
	int i = (int)index;
	float frac = index - i;

	// Linear interpolation
	int i0 = i & (SINE_TABLE_SIZE - 1); // Wrap around
	int i1 = (i0 + 1) & (SINE_TABLE_SIZE - 1);
	return sine_table[i0] + frac * (sine_table[i1] - sine_table[i0]);
}

//float clib__sinf1(float x) {
//    if (is_sine_table_initialized == FALSE)
//    {
//        init_sine_table();
//        is_sine_table_initialized = TRUE;
//    }
//
//    // Normalize angle to [0, 2π]
//    x = clib__fmod(x, 2.0 * PI);
//    if (x < 0) x += 2.0 * PI;
//
//    // Scale to table index
//    float index = x * (SINE_TABLE_SIZE / (2.0 * PI));
//    int i = (int)index;
//    float frac = index - i;
//
//    // Cubic interpolation using four table points
//    int i0 = (i - 1) & (SINE_TABLE_SIZE - 1); // Previous point
//    int i1 = i & (SINE_TABLE_SIZE - 1);       // Current point
//    int i2 = (i + 1) & (SINE_TABLE_SIZE - 1);  // Next point
//    int i3 = (i + 2) & (SINE_TABLE_SIZE - 1);  // Next-next point
//
//    float p0 = sine_table[i0];
//    float p1 = sine_table[i1];
//    float p2 = sine_table[i2];
//    float p3 = sine_table[i3];
//
//    // Cubic interpolation coefficients
//    float frac2 = frac * frac;
//    float frac3 = frac2 * frac;
//    float a0 = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
//    float a1 = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
//    float a2 = -0.5 * p0 + 0.5 * p2;
//    float a3 = p1;
//
//    return a0 * frac3 + a1 * frac2 + a2 * frac + a3;
//}

float clib__cosf(float x)
{
	return clib__sinf(x + MATH_PI / 2.0f);
}

float clib__tanf(float x)
{
	x = clib__fmod(x, 2.0 * PI);
	if (x < 0)
	{
		x += 2.0 * PI;
	}
	float sin_x = clib__sinf(x);
	float cos_x = clib__cosf(x);
	if (clib__fabs(cos_x) < 1e-6)
	{
		return copysign_c(1e6, sin_x);
	}
	return sin_x / cos_x;
}