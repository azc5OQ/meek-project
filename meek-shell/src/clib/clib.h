#ifndef CLIB_H
#define CLIB_H 1

long clib__strtol(const char *nptr, char **endptr, int base);
void clib__str_to_float(float f, char *str, ubyte precision);
void clib__merge_wstr(ubyte *first, ubyte *second);
void clib__split_str_must_call_after_usage(int number_of_strings, char **strings);
char **clib__split_str(char *string_to_split, int string_to_split_length, char *substring_used_in_splitting, int substring_used_in_splitting_length, int *splitted_substrings_count);
// boole				clib__is_str_number(char*
// string_to_check); char* clib__int_to_hex_string(int n); char*
// clib__int_to_string(int i, char* b);
nuint clib__string_length(cstring arg_string);
nuint clib__widestring_length(wchar_t *arg_string);
void clib__read_string_from_memory_and_save_to_location(char *memory_to_extract_from, char *location_to_save_string_to);
void clib__custom_memory_copy(ubyte *source, ubyte *destination, int source_offset, int destination_offset, int size);
boole clib__is_string_equal_to_another_string(cstring str1, cstring str2);
boole clib__is_wide_string_equal_to_another_wide_string(wchar_t *str1, wchar_t *str2); // nedokoncene
boole clib__is_address_pointing_to_string(char *string); // zisti ci je nieco string
void clib__convert_hexstring_to_longlong(nuint *output, char *hexstring);
const char *clib__strstr(const char *s1,
	const char *s2); // zisti ci sa v stringu nachazda iny string
int clib__strcmp(const char *str1,
	const char *str2); // navrati 0 ak sa stringy rovnaju.
int clib__strncmp(const char *str1, const char *str2,
	int number_of_bytes_to_compare); // navrati 0 ak sa stringy rovnaju.
int clib__stricmp(const char *str1,
	const char *str2); // navrati 0 ak sa stringy rovnaju, avsak nezalezi na tom
// ci su velke alebo male pismenko
boole clib__string_to_boole(char *string);

// ok, detours k tomuto potreboval fastcall__ stdcall padal.

boole clib__is_wstring_equal_to_another_wstring_ignore_case(wchar_t *str1, wchar_t *str2);
boole clib__is_wstring_equal_to_another_wstring(wchar_t *str1, wchar_t *str2);

boole clib__is_little_endian(void);
void clib__print_block_of_memory(void *base, nint length);
void clib__print_bits_of_byte(ubyte value_to_print);
void clib__print_bits_of_uint64(nuint value_to_print);

nuint clib__find_address_in_module_address_space_use_pattern(HMODULE moduleHandle, cstring pattern, nuint offset);
nuint clib__find_address_in_module_address_space_use_pattern_my_version(HMODULE moduleHandle, cstring pattern, nuint offset);
uint clib__find_offset_in_buffer_use_pattern(uint start_offset, ubyte *buffer, uint size_of_address_space_to_search_in, ubyte *bytes_to_find, uint buffer_length);
nuint clib__find_address_in_buffer_use_pattern(uint start_offset, ubyte *buffer, uint size_of_address_space_to_search_in, ubyte *bytes_to_find, uint buffer_length);
nuint clib__find_address_use_pattern_and_use_mask(cstring pattern, cstring mask, nuint start, nuint end, nuint offset);
nuint clib__find_address_use_pattern(cstring pattern, nuint start, nuint end, nuint offset);
nuint clib__find_push_instruction(nuint start, nuint end, cstring Message);

// existuju aj SSE2/ SIMD scannery, checkni osiris hack

ubyte clib__remove_flag(ubyte arg_value1, ubyte flag);
// byte				clib__set_bit_off(byte arg_value1, byte
// bit_position);
boole clib__is_bit_active_in_uint64(uint64 value, int bit_index);
boole clib__is_bit_active_in_uint32(uint value,
	int bit_index); // bit index of 0, LSB bit
uint clib__hex2int(char *hex);
uint64 clib__hexstring_to_uint64(char *hex_string);

nuint clib__find_reference(nuint start, nuint end, nuint address);
boole clib__is_address_within_bounds(nuint address_to_check, nuint from, nuint to);

// in LLVM-clang` its possible to do this

#ifdef ARCHITECTURE_I386
void *__cdecl memcpy(void *destination, void const *src, size_t count);
void *__cdecl memset(void *dest, int c, size_t count);
void *__cdecl memmove(void *destination, void const *source,
	size_t length); /// nazov tejto metody zda sa nemoze byt iny..
#endif

#ifdef ARCHITECTURE_AMD64
void *__cdecl memcpy(void *destination, void const *src, size_t count);
void *__cdecl memset(void *dest, int c, size_t count);
void *__cdecl memmove(void *destination, void const *source,
	size_t length); /// nazov tejto metody zda sa nemoze byt iny..
#endif

void clib__null_memory(void *source, nuint length);
void clib__copy_memory(void *source, void *destination, nint length, nint max_allowed_length);
boole clib__memory_compare(const void *ptr1, const void *ptr2, uint num);
void clib__free_internal_heap_handle(void);

float clib__atof(char *pstr);
int clib__abs(int x);
double clib__dabs(double x);
float clib__fabs(float value); // absolute value
double clib__sqrt(double x);
float clib__sqrtf(float x);
void clib__uint64_to_ansi_string(uint64 value, char *buffer);
void clib__int64_to_ansi_string(int64 value, char *buffer);
double clib__pow(double base, double exponent);
double clib__acos(double x);
float clib__acosf(float x);
void clib__ftoa(float number, char *str); // float to ansi string
void clib__ftoa_1(float number, char *str, int precision);
double clib__atan2(double x, double y);
double clib__atan(double x);
float clib__sinf(float x);
float clib__cosf(float x); // accuracy depeends on clib__sinf
float clib__tanf(float x); // accuracy depeends on clib__sinf
float clib__math_get_sinus_from_rad_angle(float radian_angle);
float clib__cos(float radian_angle);
double clib__floor64(double x);
float clib__floor32(float x); // tato funkcia nieje o zaokruhlovani, aj ked maze sa pri jej
// pouzity z hodnoty desatinna ciarka. floor prijme realne cislo,
// a navrati prve mensie cislo prirozdene,
float clib__ceil(float f); // ceil je funkcia pre zaokruhlenie
float clib__fmod(float x, float y); // modulo operacia, ked sa x podeli y, aky bude zvysok?
int clib__truncate(float x); // truncate navrati hodnotu bez destatinnej ciarky
double clib__ln(const double n); // na kolko sa musi umocnit eulerovo cislo aby
// som dostal n?
double clib__log(const double n,
	const double base); // na kolko sa musi umocnit base aby som dostal n?
uint clib__modulo(uint divident, uint divisor);
void clib__zerostring(char *Str);

// bool_				clib__create_file_from_buffer(char*
// new_file_name, byte* buffer, size_t length);

// void				clib__qsort(); toto som nevedel implementovat,
// na neskor. zatial pouzivam qsort s mvscrt, aj ked msvcrt dost pada
typedef int (*p_fnc_clib_qsort_comparator)(const void *, const void *);
void clib__qsort(void *base, uint64 num_of_elements, uint64 size_of_element, p_fnc_clib_qsort_comparator compare);

boole clib__read_file(cstring filename, ubyte **out_buffer, uint64 *output_file_buffer_length);

#endif
