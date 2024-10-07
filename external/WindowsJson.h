#pragma once

#include <windows.h>
#include <windows.data.json.h>

#include <stdint.h>
#include <stdbool.h>

// interface

typedef __x_ABI_CWindows_CData_CJson_CIJsonObject JsonObject;
typedef __x_ABI_CWindows_CData_CJson_CIJsonArray  JsonArray;

typedef __FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue JsonIterator;

#define JsonCSTR(Name) (HSTRING)&(JsonHSTRING){ 1, sizeof(Name) - 1, 0, 0, L ## Name }
#define JsonRelease(Obj) do { if (Obj) IUnknown_Release((IUnknown*)Obj); } while (0)

// NOTE: make sure you have called RoInitialize() before using any of functions

// if zero terminated, can set Length to -1
static JsonObject* JsonObject_Parse(LPCSTR String, int Length);
static JsonObject* JsonObject_ParseW(LPCWSTR String, int Length);
static JsonObject* JsonObject_ParseStr(HSTRING String);

static JsonObject* JsonObject_GetObject (JsonObject* Object, HSTRING Name);
static JsonArray*  JsonObject_GetArray  (JsonObject* Object, HSTRING Name);
static HSTRING     JsonObject_GetString (JsonObject* Object, HSTRING Name);
static double      JsonObject_GetNumber (JsonObject* Object, HSTRING Name);
static bool        JsonObject_GetBoolean(JsonObject* Object, HSTRING Name);

static JsonIterator* JsonObject_GetIterator(JsonObject* Object);
static HSTRING       JsonIterator_GetKey  (JsonIterator* Iterator);
static JsonObject*   JsonIterator_GetValue(JsonIterator* Iterator);
static bool          JsonIterator_Next    (JsonIterator* Iterator);

static uint32_t    JsonArray_GetCount  (JsonArray* Array);
static JsonObject* JsonArray_GetObject (JsonArray* Array, UINT32 Index);
static JsonArray*  JsonArray_GetArray  (JsonArray* Array, UINT32 Index);
static HSTRING     JsonArray_GetString (JsonArray* Array, UINT32 Index);
static double      JsonArray_GetNumber (JsonArray* Array, UINT32 Index);
static bool        JsonArray_GetBoolean(JsonArray* Array, UINT32 Index);

// implementation

#ifndef Assert
#define Assert(Cond) ((void)(Cond))
#endif

#ifndef HR
#define HR(hr) ((void)(hr))
#endif

typedef struct {
	DWORD Flags;
	DWORD Length;
	DWORD Padding1;
	DWORD Padding2;
	LPCWSTR Ptr;
} JsonHSTRING;

DEFINE_GUID(IID_IJsonObject,        0x2289f159, 0x54de, 0x45d8, 0xab, 0xcc, 0x22, 0x60, 0x3f, 0xa0, 0x66, 0xa0);
DEFINE_GUID(IID_IVector_IJsonValue, 0xd44662bc, 0xdce3, 0x59a8, 0x92, 0x72, 0x4b, 0x21, 0x0f, 0x33, 0x90, 0x8b);
//DEFINE_GUID(IID_IMap_IJsonValue,    0xf948eac5, 0x33eb, 0x50f5, 0xb5, 0xaf, 0xe7, 0xce, 0xcf, 0x0e, 0x45, 0x01);
DEFINE_GUID(IID_IMap_IJsonValue,    0xdfabb6e1, 0x0411, 0x5a8f, 0xaa, 0x87, 0x35, 0x4e, 0x71, 0x10, 0xf0, 0x99);

JsonObject* JsonObject_Parse(LPCSTR String, int Length)
{
	if (Length < 0) Length = lstrlenA(String);
	int WideLength = MultiByteToWideChar(CP_UTF8, 0, String, Length, NULL, 0);
	Assert(WideLength != 0);

	WCHAR* Wide = LocalAlloc(0, WideLength * sizeof(WCHAR));
	MultiByteToWideChar(CP_UTF8, 0, String, Length, Wide, WideLength);

	JsonObject* Result = JsonObject_ParseW(Wide, WideLength);

	LocalFree(Wide);

	return Result;
}

JsonObject* JsonObject_ParseW(LPCWSTR String, int Length)
{
	JsonHSTRING WideString = { 1, Length < 0 ? lstrlenW(String) : Length, 0, 0, String };
	return JsonObject_ParseStr((HSTRING)&WideString);
}

JsonObject* JsonObject_ParseStr(HSTRING String)
{
	__x_ABI_CWindows_CData_CJson_CIJsonObjectStatics* ObjectStatics;
	HR(RoGetActivationFactory(JsonCSTR("Windows.Data.Json.JsonObject"), &IID_IJsonObject, (void**)&ObjectStatics));

	JsonObject* Object;
	HRESULT hr;
	if (FAILED(hr = __x_ABI_CWindows_CData_CJson_CIJsonObjectStatics_Parse(ObjectStatics, String, &Object)))
	{
		Object = NULL;
	}
	__x_ABI_CWindows_CData_CJson_CIJsonObjectStatics_Release(ObjectStatics);

	return Object;
}

JsonObject* JsonObject_GetObject(JsonObject* Object, HSTRING Name)
{
	JsonObject* Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedObject(Object, Name, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

JsonArray* JsonObject_GetArray(JsonObject* Object, HSTRING Name)
{
	JsonArray* Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedArray(Object, Name, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

HSTRING JsonObject_GetString(JsonObject* Object, HSTRING Name)
{
	HSTRING Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedString(Object, Name, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

double JsonObject_GetNumber(JsonObject* Object, HSTRING Name)
{
	double Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedNumber(Object, Name, &Result)))
	{
		Result = 0;
	}
	return Result;
}

bool JsonObject_GetBoolean(JsonObject* Object, HSTRING Name)
{
	boolean Result;
	if (!Object || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonObject_GetNamedBoolean(Object, Name, &Result)))
	{
		Result = 0;
	}
	return !!Result;
}

JsonIterator* JsonObject_GetIterator(JsonObject* Object)
{
	__FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue* Iterator = NULL;

	__FIIterable_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue* Iterable;
	if (Object && SUCCEEDED(__x_ABI_CWindows_CData_CJson_CIJsonObject_QueryInterface(Object, &IID_IMap_IJsonValue, (void**)&Iterable)))
	{
		HR(__FIIterable_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_First(Iterable, &Iterator));

		boolean HasCurrent;
		HR(__FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_get_HasCurrent(Iterator, &HasCurrent));
		if (!HasCurrent)
		{
			__FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_Release(Iterator);
			Iterator = NULL;
		}

		__FIIterable_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_Release(Iterable);
	}

	return Iterator;
}

HSTRING JsonIterator_GetKey(JsonIterator* Iterator)
{
	HSTRING Result = NULL;

	__FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue* Current;
	if (Iterator && SUCCEEDED(__FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_get_Current(Iterator, &Current)))
	{
		HR(__FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_get_Key(Current, &Result));
		__FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_Release(Current);
	}

	return Result;
}

JsonObject* JsonIterator_GetValue(JsonIterator* Iterator)
{
	JsonObject* Result = NULL;

	__FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue* Current;
	if (Iterator && SUCCEEDED(__FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_get_Current(Iterator, &Current)))
	{
		__x_ABI_CWindows_CData_CJson_CIJsonValue* Value;
		HR(__FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_get_Value(Current, &Value));
		__FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_Release(Current);

		HR(__x_ABI_CWindows_CData_CJson_CIJsonValue_GetObject(Value, &Result));
		__x_ABI_CWindows_CData_CJson_CIJsonValue_Release(Value);
	}

	return Result;
}

bool JsonIterator_Next(JsonIterator* Iterator)
{
	boolean Result;
	if (Iterator && FAILED(__FIIterator_1___FIKeyValuePair_2_HSTRING_Windows__CData__CJson__CIJsonValue_MoveNext(Iterator, &Result)))
	{
		Result = 0;
	}
	return !!Result;
}

uint32_t JsonArray_GetCount(JsonArray* Array)
{
	if (!Array)
	{
		return 0;
	}

	UINT32 Count;
	
	__FIVector_1_Windows__CData__CJson__CIJsonValue* Vector;
	HR(__x_ABI_CWindows_CData_CJson_CIJsonArray_QueryInterface(Array, &IID_IVector_IJsonValue, (void**)&Vector));
	if (FAILED(__FIVector_1_Windows__CData__CJson__CIJsonValue_get_Size(Vector, &Count)))
	{
		Count = 0;
	}
	__FIVector_1_Windows__CData__CJson__CIJsonValue_Release(Vector);

	return Count;
}

JsonObject* JsonArray_GetObject(JsonArray* Array, UINT32 Index)
{
	JsonObject* Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetObjectAt(Array, Index, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

JsonArray* JsonArray_GetArray(JsonArray* Array, UINT32 Index)
{
	JsonArray* Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetArrayAt(Array, Index, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

HSTRING JsonArray_GetString(JsonArray* Array, UINT32 Index)
{
	HSTRING Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetStringAt(Array, Index, &Result)))
	{
		Result = NULL;
	}
	return Result;
}

double JsonArray_GetNumber(JsonArray* Array, UINT32 Index)
{
	double Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetNumberAt(Array, Index, &Result)))
	{
		Result = 0;
	}
	return Result;
}

bool JsonArray_GetBoolean(JsonArray* Array, UINT32 Index)
{
	boolean Result;
	if (!Array || FAILED(__x_ABI_CWindows_CData_CJson_CIJsonArray_GetBooleanAt(Array, Index, &Result)))
	{
		Result = 0;
	}
	return !!Result;
}
