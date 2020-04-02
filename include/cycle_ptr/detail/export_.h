#ifndef CYCLE_PTR_DETAIL_EXPORT__H
#define CYCLE_PTR_DETAIL_EXPORT__H

/*
 * Various macros to control symbol visibility in libraries.
 */
#if defined(WIN32)
# ifdef cycle_ptr_EXPORTS
#   define cycle_ptr_export_  __declspec(dllexport)
#   define cycle_ptr_local_   /* nothing */
# else
#   define cycle_ptr_export_  __declspec(dllimport)
#   define cycle_ptr_local_   /* nothing */
# endif
#elif defined(__GNUC__) || defined(__clang__)
# define cycle_ptr_export_    __attribute__ ((visibility ("default")))
# define cycle_ptr_local_     __attribute__ ((visibility ("hidden")))
#else
# define cycle_ptr_export_    /* nothing */
# define cycle_ptr_local_     /* nothing */
#endif

#endif /* CYCLE_PTR_DETAIL_EXPORT__H */
