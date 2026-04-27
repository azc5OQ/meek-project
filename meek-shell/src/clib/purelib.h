#ifndef PURELIB_H
#define PURELIB_H

// this only contains standard functions, no external dependencies
// these functions try to be close to originals. They try to be but they are not

// add comments

/**
 * @brief converts string to long
 *
 * @param const char *str -> a pointer to the null-terminated string to be
 * converted
 * @param char **endptr
 * @param int base -> a base of a number to be converted (function needs this to
 * tell if string number is hexadecimal or decimal)
 *
 *
 * @return long
 */
long purelib__strtol(const char *str, char **endptr, int base);

/**
 * @brief
 *
 * @param const wchar_t *str
 *
 *
 * @return size_t
 */
size_t purelib__wcslen(const wchar_t *str);

/**
 * @brief retrieves string length
 *
 * @param const char *str
 *
 *
 * @return size_t
 */
size_t purelib__strlen(const char *str);

/**
 * @brief compares two strings, returns 0 (FALSE) if they are EQUAL
 *
 * @param const char *str1
 * @param const char *str2
 *
 *
 * @return int
 */
int purelib__strcmp(const char *str1, const char *str2);

/**
 * @brief compares two strings, returns 0 (FALSE) if they are EQUAL, difference
 * between strcmp is that this also specifies max number of chars to compare
 *
 * @param const char *str1
 * @param const char *str2
 * @param size_t n
 *
 *
 * @return int
 */
int purelib__strncmp(const char *str1, const char *str2,
	size_t n); // compares specific number of characters,
// //returns 0 if strings are equal

/**
 * @brief compares two strings, returns 0 (FALSE) if they are EQUAL, ignores the
 * case (doesnt matter if big or small letters)
 *
 * @param const char *str1
 * @param const char *str2
 *
 *
 * @return int
 */
int purelib__stricmp(const char *str1, const char *str2);

/**
 * @brief compares two wide strings, returns 0 (FALSE) if they are EQUAL
 *
 * @param const wchar_t *str1
 * @param const wchar_t *str2
 *
 * @return int
 */
int purelib__wcscmp(const wchar_t *str1, const wchar_t *str2);

void purelib__null_memory(void *source, size_t length);
void purelib__copy_memory(void *source, void *destination, size_t length, size_t max_allowed_length);

/**
 * @brief returns smaller value
 *
 * @param int a
 * @param int b
 *
 * @return int
 */
int purelib__min(int a, int b);
uint64 purelib__min_64(uint64 a, uint64 b);

#endif
