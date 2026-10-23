#ifndef EDGEFLOW_EXPORT_H_
#define EDGEFLOW_EXPORT_H_

#if defined(EDGEFLOW_STATIC_LINK)
#define EDGEFLOW_API
#elif defined(_WIN32)
#if defined(EDGEFLOW_BUILDING_SDK)
#define EDGEFLOW_API __declspec(dllexport)
#else
#define EDGEFLOW_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define EDGEFLOW_API __attribute__((visibility("default")))
#else
#define EDGEFLOW_API
#endif

#endif  // EDGEFLOW_EXPORT_H_
