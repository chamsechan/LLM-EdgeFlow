#ifndef COMPANY_ALG_EXPORT_H_
#define COMPANY_ALG_EXPORT_H_

#if defined(COMPANY_ALG_STATIC_LINK)
#define COMPANY_ALG_API
#elif defined(_WIN32)
#if defined(COMPANY_ALG_BUILDING_SDK)
#define COMPANY_ALG_API __declspec(dllexport)
#else
#define COMPANY_ALG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define COMPANY_ALG_API __attribute__((visibility("default")))
#else
#define COMPANY_ALG_API
#endif

#endif  // COMPANY_ALG_EXPORT_H_
