#if defined(__MSVC__) || defined(__MINGW32__) || defined(__MINGW64__)
	#ifdef ZORM_LIB
		#define ZORM_API __declspec(dllexport)
	#else
		#define ZORM_API __declspec(dllimport)
	#endif
#elif defined(__LINUX__)
	#ifdef ZORM_LIB
		#define ZORM_API __attribute__ ((visibility ("default"))) 
	#else
		#define ZORM_API
	#endif
#else
    #define ZORM_API
#endif