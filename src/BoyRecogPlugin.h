#pragma once
#include <stddef.h>
#include <stdint.h>

typedef uint64_t BoyRecogCapabilities;
typedef int32_t BoyRecogItemType;
typedef int32_t BoyRecogDescriptionDetail;
enum : BoyRecogCapabilities {
    BOY_RECOG_CAP_OCR = 1ULL, BOY_RECOG_CAP_AUTO_OCR = 2ULL,
    BOY_RECOG_CAP_PDF_OCR = 4ULL, BOY_RECOG_CAP_OCR_POSITION = 32ULL,
    BOY_RECOG_CAP_OCR_OTHER_TYPES = 64ULL, BOY_RECOG_CAP_IMAGE_BRIEF = 512ULL,
    BOY_RECOG_CAP_IMAGE_DETAILED = 1024ULL, BOY_RECOG_CAP_ICON = 8192ULL
};
enum : BoyRecogItemType {
    BOY_RECOG_ITEM_ICON = 0, BOY_RECOG_ITEM_ICON_WITH_LABEL = 1,
    BOY_RECOG_ITEM_TEXT = 3, BOY_RECOG_ITEM_IMAGE = 4
};
enum : BoyRecogDescriptionDetail {
    BOY_RECOG_DESCRIPTION_BRIEF = 0, BOY_RECOG_DESCRIPTION_DETAILED = 1
};
#pragma pack(push, 8)
struct BoyRecogEngineInfo { const wchar_t* name; BoyRecogCapabilities capabilities; };
struct BoyRecogRect { int32_t left, top, right, bottom; };
struct BoyRecogCharInfo { const wchar_t* text; BoyRecogRect box; };
#pragma pack(pop)
typedef void (__cdecl* BoyRecogEngineListCallback)(const BoyRecogEngineInfo*, size_t, void*);
typedef void (__cdecl* BoyRecogTextCallback)(const wchar_t*, void*);
typedef void (__cdecl* BoyRecogItemCallback)(BoyRecogItemType, const wchar_t*,
    const BoyRecogRect*, const BoyRecogCharInfo*, size_t, void*);
extern "C" {
int __cdecl BoyRecogInit();
int __cdecl BoyRecogUninit();
int __cdecl BoyRecogSettings(const wchar_t* engineName);
int __cdecl BoyRecogGetEngineList(BoyRecogEngineListCallback, void*);
int __cdecl BoyRecogOcrText(const wchar_t*, const char*, size_t, BoyRecogTextCallback, void*);
int __cdecl BoyRecogOcrItems(const wchar_t*, const char*, size_t, int, BoyRecogItemCallback, void*);
int __cdecl BoyRecogDescribeImage(const wchar_t*, const char*, size_t, BoyRecogDescriptionDetail, BoyRecogTextCallback, void*);
int __cdecl BoyRecogRecognizeIcon(const wchar_t*, const char*, size_t, BoyRecogTextCallback, void*);
}
