//
// By using this Software, you are accepting original [LZMA SDK] and MIT license below:
//
// The MIT License (MIT)
//
// Copyright (c) 2015 - 2026 Oleh Kulykov <olehkulykov@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//


#include <cstddef>
#include <climits>
#include <cstdint>

#include "plzma_base_callback.hpp"
#include "plzma_c_bindings_private.hpp"

#include "CPP/7zip/Archive/DllExports2.h"

namespace plzma {

#if WCHAR_MAX > 0xFFFF
    static HRESULT PasswordStringToBstr(const String & string, BSTR * output) {
        const wchar_t * source = string.wide();
        UINT utf16Length = 0;
        for (size_t index = 0; source[index] != 0; index++) {
            const uint32_t scalar = static_cast<uint32_t>(source[index]);
            utf16Length += scalar > 0xFFFF && scalar <= 0x10FFFF ? 2 : 1;
        }
        BSTR result = ::SysAllocStringLen(nullptr, utf16Length);
        if (!result) {
            return E_OUTOFMEMORY;
        }
        UINT destinationIndex = 0;
        for (size_t index = 0; source[index] != 0; index++) {
            uint32_t scalar = static_cast<uint32_t>(source[index]);
            if (scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) {
                scalar = 0xFFFD;
            }
            if (scalar > 0xFFFF) {
                scalar -= 0x10000;
                result[destinationIndex++] = static_cast<wchar_t>(0xD800 + (scalar >> 10));
                result[destinationIndex++] = static_cast<wchar_t>(0xDC00 + (scalar & 0x3FF));
            } else {
                result[destinationIndex++] = static_cast<wchar_t>(scalar);
            }
        }
        *output = result;
        return S_OK;
    }
#else
    static HRESULT PasswordStringToBstr(const String & string, BSTR * output) {
        return StringToBstr(string.wide(), output);
    }
#endif
    
    HRESULT BaseCallback::getTextPassword(Int32 * passwordIsDefined, BSTR * password) noexcept {
#if defined(LIBPLZMA_NO_CRYPTO)
        LIBPLZMA_SET_VALUE_TO_PTR(passwordIsDefined, BoolToInt(false))
#else
        try {
            LIBPLZMA_LOCKGUARD(lock, _mutex)
            if (_result != S_OK) {
                return _result;
            }
            _passwordRequested = true;
            if (_password.count() > 0) {
                _result = PasswordStringToBstr(_password, password);
                LIBPLZMA_SET_VALUE_TO_PTR(passwordIsDefined, BoolToInt(_result == S_OK))
            } else {
                LIBPLZMA_SET_VALUE_TO_PTR(passwordIsDefined, BoolToInt(false))
            }
            return _result;
        } catch (const Exception & exception) {
            _exception = exception.moveToHeapCopy();
            return E_FAIL;
        }
#if defined(LIBPLZMA_HAVE_STD)
        catch (const std::exception & exception) {
            _exception = Exception::create(plzma_error_code_internal, exception.what(), __FILE__, __LINE__);
            return E_FAIL;
        }
#endif
        catch (...) {
            _exception = Exception::create(plzma_error_code_not_enough_memory, "Can't convert string to a binary string.", __FILE__, __LINE__);
            return E_FAIL;
        }
#endif
        return S_OK;
    }
    
#if !defined(LIBPLZMA_NO_PROGRESS)
    HRESULT BaseCallback::setProgressTotal(const uint64_t total) noexcept {
        try {
            LIBPLZMA_LOCKGUARD(lock, _mutex)
            if (_result == S_OK) {
                _progress->setTotal(total);
            }
            return _result;
        } catch (const Exception & exception) {
            _exception = exception.moveToHeapCopy();
            return E_FAIL;
        }
#if defined(LIBPLZMA_HAVE_STD)
        catch (const std::exception & exception) {
            _exception = Exception::create(plzma_error_code_internal, exception.what(), __FILE__, __LINE__);
            return E_FAIL;
        }
#endif
        catch (...) {
            _exception = Exception::create(plzma_error_code_internal, "Can't set progress total.", __FILE__, __LINE__);
            return E_FAIL;
        }
        return S_OK;
    }
    
    HRESULT BaseCallback::setProgressCompleted(const uint64_t completed) noexcept {
        try {
            LIBPLZMA_LOCKGUARD(lock, _mutex)
            if (_result == S_OK) {
                _progress->setCompleted(completed);
            }
            return _result;
        } catch (const Exception & exception) {
            _exception = exception.moveToHeapCopy();
            return E_FAIL;
        }
#if defined(LIBPLZMA_HAVE_STD)
        catch (const std::exception & exception) {
            _exception = Exception::create(plzma_error_code_internal, exception.what(), __FILE__, __LINE__);
            return E_FAIL;
        }
#endif
        catch (...) {
            _exception = Exception::create(plzma_error_code_internal, "Can't set progress completed.", __FILE__, __LINE__);
            return E_FAIL;
        }
        return S_OK;
    }
#endif // !LIBPLZMA_NO_PROGRESS

    BaseCallback::~BaseCallback() {
        delete _exception;
        _exception = nullptr; // virtual base
#if !defined(LIBPLZMA_NO_CRYPTO)
        _password.clear(plzma_erase_zero);
#endif
    }
    
    static GUID CLSIDType7z(void) noexcept {
        return CONSTRUCT_GUID(0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00);
    }
    
    static GUID CLSIDTypeXz(void) noexcept {
        return CONSTRUCT_GUID(0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, 0x0C, 0x00, 0x00);
    }
    
#if !defined(LIBPLZMA_NO_TAR)
    static GUID CLSIDTypeTar(void) noexcept {
        return CONSTRUCT_GUID(0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, 0xEE, 0x00, 0x00);
    }
#endif
    
    template<typename T>
    inline CMyComPtr<T> createArchiveWithGUID(const GUID * archiveGUID, const plzma_file_type type) {
        HRESULT res = S_FALSE;
        T * ptr = nullptr;
        CMyComPtr<T> sptr;
        switch (type) {
            case plzma_file_type_7z: {
                const GUID clsid7z = CLSIDType7z();
                res = CreateObject(&clsid7z, archiveGUID, reinterpret_cast<void**>(&ptr));
                break;
            }
            case plzma_file_type_xz: {
                const GUID clsidXz = CLSIDTypeXz();
                res = CreateObject(&clsidXz, archiveGUID, reinterpret_cast<void**>(&ptr));
                break;
            }
            case plzma_file_type_tar: {
#if defined(LIBPLZMA_NO_TAR)
                throw Exception(plzma_error_code_invalid_arguments, LIBPLZMA_NO_TAR_EXCEPTION_WHAT, __FILE__, __LINE__);
#else
                const GUID clsidTar = CLSIDTypeTar();
                res = CreateObject(&clsidTar, archiveGUID, reinterpret_cast<void**>(&ptr));
#endif
                break;
            }
            default: break;
        }
        sptr.Attach(ptr);
        if ((res == S_OK) && ptr) {
            return sptr;
        }
        Exception exception(plzma_error_code_internal, "Can't create archive object.", __FILE__, __LINE__);
        exception.setReason("Unsupported type or codec/plugin not compiled in.", nullptr);
        throw exception;
    }
    
    template<>
    CMyComPtr<IInArchive> BaseCallback::createArchive(const plzma_file_type type) {
        return createArchiveWithGUID<IInArchive>(&IID_IInArchive, type);
    }
    
    template<>
    CMyComPtr<IOutArchive> BaseCallback::createArchive(const plzma_file_type type) {
        return createArchiveWithGUID<IOutArchive>(&IID_IOutArchive, type);
    }
}
