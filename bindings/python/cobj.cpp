// Copyright 2018 Adrien Guinet <adrien@guinet.me>
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdlib>
#include "cobj.h"
#include "dispatcher.h"
#include "errors.h"

#include <dffi/casting.h>
#include <dffi/types.h>

namespace py = pybind11;
using namespace dffi;

namespace {

struct ValueSetter
{
  // TODO: assert alignment is correct!
  template <class T>
  static void case_basic(BasicType const* Ty, void* Ptr, py::handle Obj)
  {
    T* TPtr = reinterpret_cast<T*>(Ptr);
    *TPtr = Obj.cast<T>();
  }

  static void case_pointer(PointerType const* Ty, void* Ptr, py::handle Obj)
  {
    CPointerObj const& PtrObj = Obj.cast<CPointerObj const&>();
    void** PPtr = reinterpret_cast<void**>(Ptr);
    *PPtr = PtrObj.getPtr();
  }

  template <class T>
  static void case_composite(T const* Ty, void* Ptr, py::handle Obj)
  {
    using CObjTy = typename std::conditional<std::is_same<T, StructType>::value, CStructObj, CUnionObj>::type;
    auto const& CObj = Obj.cast<CObjTy const&>();
    memcpy(Ptr, CObj.getData(), CObj.getSize());
  }

  static void case_enum(EnumType const* Ty, void* Ptr, py::handle Obj)
  {
    return case_basic<EnumType::IntType>(Ty->getBasicType(), Ptr, Obj);
  }

  static void case_array(ArrayType const* Ty, void* Ptr, py::handle Obj)
  {
    CArrayObj const& ArrayObj = Obj.cast<CArrayObj const&>();
    memcpy(Ptr, ArrayObj.getData(), Ty->getSize());
  }

  static void case_func(FunctionType const* Ty, void* Ptr, py::handle Obj)
  {
    // This should never happen, as this is prevented by the C standard!
    // Raise an exception!
    throw TypeError{"unable to set a value to a function!"};
  }
};

struct ValueGetter
{
  // TODO: assert alignment is correct!
  template <class T>
  static py::object case_basic(BasicType const* Ty, void* Ptr)
  {
    return py::cast(*reinterpret_cast<T*>(Ptr));
  }

  static py::object case_enum(EnumType const* Ty, void* Ptr)
  {
    return py::cast(*reinterpret_cast<EnumType::IntType*>(Ptr));
  }

  static py::object case_pointer(PointerType const* Ty, void* Ptr)
  {
    auto* Ret = new CPointerObj{*Ty, Data<void*>::view((void**)Ptr)};
    return py::cast(Ret, py::return_value_policy::take_ownership);
  }

  template <class T, class CT>
  static py::object case_composite_impl(CT const* Ty, void* Ptr)
  {
    auto* Ret = new T{*Ty, Data<void>::view(Ptr)};
    return py::cast(Ret, py::return_value_policy::take_ownership);
  }

  static py::object case_composite(StructType const* Ty, void* Ptr)
  {
    return case_composite_impl<CStructObj>(Ty, Ptr);
  }

  static py::object case_composite(UnionType const* Ty, void* Ptr)
  {
    return case_composite_impl<CUnionObj>(Ty, Ptr);
  }

  static py::object case_array(ArrayType const* Ty, void* Ptr)
  {
    auto* Ret = new CArrayObj{*Ty, Data<void>::view(Ptr)};
    return py::cast(Ret, py::return_value_policy::take_ownership);
  }

  static py::object case_func(FunctionType const* Ty, void* Ptr)
  {
    // This should never happen, as this is prevented by the C standard!
    // Raise an exception!
    throw TypeError{"unable to get a value as a function!"};
  }
};

struct PtrToObjView
{
  template <class T>
  static std::unique_ptr<CObj> case_basic(BasicType const* Ty, void* Ptr)
  {
    auto* TPtr = reinterpret_cast<T*>(Ptr);
    return std::unique_ptr<CObj>{new CBasicObj<T>{*Ty, Data<T>::view(TPtr)}};
  }

  static std::unique_ptr<CObj> case_enum(EnumType const* Ty, void* Ptr)
  {
    return case_basic<EnumType::IntType>(Ty->getBasicType(), Ptr);
  }

  static std::unique_ptr<CObj> case_pointer(PointerType const* Ty, void* Ptr)
  {
    return std::unique_ptr<CObj>{new CPointerObj{*Ty, Data<void*>::view((void**)Ptr)}};
  }

  static std::unique_ptr<CObj> case_composite(StructType const* Ty, void* Ptr)
  {
    return std::unique_ptr<CObj>{new CStructObj{*Ty, Data<void>::view(Ptr)}};
  }

  static std::unique_ptr<CObj> case_composite(UnionType const* Ty, void* Ptr)
  {
    return std::unique_ptr<CObj>{new CUnionObj{*Ty, Data<void>::view(Ptr)}};
  }

  static std::unique_ptr<CObj> case_array(ArrayType const* Ty, void* Ptr)
  {
    return std::unique_ptr<CObj>{new CArrayObj{*Ty, Data<void>::view(Ptr)}};
  }

  static std::unique_ptr<CObj> case_func(FunctionType const* Ty, void* Ptr)
  {
    auto NF = Ty->getFunction(Ptr);
    return std::unique_ptr<CObj>{new CFunction{NF}};
  }
};

struct ConvertArgsSwitch
{
  typedef std::vector<std::unique_ptr<CObj>> ObjsHolder;
  typedef std::vector<py::object> PyObjsHolder;

  static CObj* checkType(CObj* O, Type const* Ty)
  {
    /*if (O->getType() != Ty) {
      throw TypeError{O->getType(), Ty};
    }*/
    return O;
  }

  template <class T>
  static CObj* case_basic(BasicType const* Ty, ObjsHolder& H, PyObjsHolder&, py::handle O)
  {
    if (CObj* Ret = O.dyn_cast<CBasicObj<T>>()) {
      return Ret;
    }
    // Create a temporary object with the python value
    auto* Ret = new CBasicObj<T>{*Ty, O.cast<T>()};
    H.emplace_back(std::unique_ptr<CObj>{Ret});
    return Ret;
  }

  static CObj* case_enum(EnumType const* Ty, ObjsHolder& H, PyObjsHolder& PyH, py::handle O)
  {
    return case_basic<EnumType::IntType>(Ty->getBasicType(), H, PyH, O);
  }

  static CObj* case_pointer(PointerType const* Ty, ObjsHolder& H, PyObjsHolder& PyH, py::handle O)
  {
    if (auto* PtrObj = O.dyn_cast<CPointerObj>()) {
      return checkType(PtrObj, Ty);
    }

    auto PteTy = Ty->getPointee();
    const bool isWritable = !PteTy.hasConst();
    // If the argument is const char* and we have a py::str, do an automatic conversion using UTF8!
    // TODO: let the user choose if this automatic conversion must happen, and the codec to use!
    if (!isWritable) {
      if (auto* BTy = dyn_cast<BasicType>(PteTy.getType())) {
        if (BTy->getBasicKind() == BasicType::Char) {
          py::handle Tmp = O;
          if (PyUnicode_Check(O.ptr())) {
            py::object Buf = py::reinterpret_steal<py::object>(PyUnicode_AsUTF8String(O.ptr()));
            if (!Buf)
              throw TypeError{"Unable to extract string contents! (encoding issue)"};
            // Keep this object for the call lifetime as we will get its
            // underlying buffer!
            PyH.emplace_back(Buf);
            Tmp = Buf;
          }
          char *Buffer = PYBIND11_BYTES_AS_STRING(Tmp.ptr());
          if (!Buffer)
            throw TypeError{"Unable to extract string contents! (invalid type)"};
          auto* Ret = new CPointerObj{*Ty, Data<void*>::emplace_owned(Buffer)};
          H.emplace_back(std::unique_ptr<CObj>{Ret});
          return Ret;
        }
      }
    }
    // Cast this as a buffer
    py::buffer B = O.cast<py::buffer>();
    py::buffer_info Info = B.request(isWritable);
    if (Info.ndim != 1) {
      ThrowError<TypeError>() << "buffer should have only one dimension, got " << Info.ndim << "!";
    }
    auto ExpectedFormat = getFormatDescriptor(PteTy);
    if (Info.format != ExpectedFormat) {
      ThrowError<TypeError>() << "buffer doesn't have the good format, got '" << Info.format << "', expected '" << ExpectedFormat << "'";
    }

    auto* Ret = new CPointerObj{*Ty, Data<void*>::emplace_owned(Info.ptr)};
    H.emplace_back(std::unique_ptr<CObj>{Ret});
    return Ret;
  }

  static CObj* case_composite(StructType const* Ty, ObjsHolder&, PyObjsHolder&, py::handle O)
  {
    return checkType(O.cast<CStructObj*>(), Ty);
  }

  static CObj* case_composite(UnionType const* Ty, ObjsHolder&, PyObjsHolder&, py::handle O)
  {
    return checkType(O.cast<CUnionObj*>(), Ty);
  }

  static CObj* case_array(ArrayType const* Ty, ObjsHolder&, PyObjsHolder&, py::handle O)
  {
    return checkType(O.cast<CArrayObj*>(), Ty);
  }

  static CObj* case_func(FunctionType const* Ty, ObjsHolder&, PyObjsHolder&, py::handle O)
  {
    return checkType(O.cast<CFunction*>(), Ty);
  }
};
using ConvertArgs = TypeDispatcher<ConvertArgsSwitch>;

struct CreateObjSwitch
{
  typedef std::vector<std::unique_ptr<CObj>> ObjsHolder;

  template <class T>
  static std::unique_ptr<CObj> case_basic(BasicType const* Ty)
  {
    return std::unique_ptr<CObj>{new CBasicObj<T>{*Ty}};
  }

  static std::unique_ptr<CObj> case_enum(EnumType const* Ty)
  {
    return std::unique_ptr<CObj>{new CBasicObj<EnumType::IntType>{*Ty->getBasicType()}};
  }

  static std::unique_ptr<CObj> case_pointer(PointerType const* Ty)
  {
    return std::unique_ptr<CObj>{new CPointerObj{*Ty}};
  }

  static std::unique_ptr<CObj> case_composite(StructType const* Ty)
  {
    return std::unique_ptr<CObj>{new CStructObj{*Ty}};
  }

  static std::unique_ptr<CObj> case_composite(UnionType const* Ty)
  {
    return std::unique_ptr<CObj>{new CUnionObj{*Ty}};
  }

  static std::unique_ptr<CObj> case_array(ArrayType const* Ty)
  {
    return std::unique_ptr<CObj>{new CArrayObj{*Ty}};
  }

  static std::unique_ptr<CObj> case_func(FunctionType const* Ty)
  {
    return std::unique_ptr<CObj>{new CFunction{Ty->getFunction(nullptr)}};
  }
};
using CreateObj = TypeDispatcher<CreateObjSwitch>;

} // anonymous

std::string getFormatDescriptor(Type const* Ty)
{
  if (auto* BTy = dffi::dyn_cast<BasicType>(Ty)) {
#define HANDLE_BASICTY(DTy, CTy)\
    case BasicType::DTy:\
      return py::format_descriptor<CTy>::format();

    switch (BTy->getBasicKind()) {
      HANDLE_BASICTY(Char, char);
      HANDLE_BASICTY(UInt8, uint8_t);
      HANDLE_BASICTY(UInt16, uint16_t);
      HANDLE_BASICTY(UInt32, uint32_t);
      HANDLE_BASICTY(UInt64, uint64_t);
      HANDLE_BASICTY(Int8, int8_t);
      HANDLE_BASICTY(Int16, int16_t);
      HANDLE_BASICTY(Int32, int32_t);
      HANDLE_BASICTY(Int64, int64_t);
      HANDLE_BASICTY(Float32, float);
      HANDLE_BASICTY(Float64, double);
      default:
      break;
    };
  }
  return std::to_string(Ty->getSize()) + "B";
}

py::object CArrayObj::get(size_t Idx) {
  return TypeDispatcher<ValueGetter>::switch_(getElementType(), GEP(Idx));
}

void CArrayObj::set(size_t Idx, py::handle Obj) {
  TypeDispatcher<ValueSetter>::switch_(getElementType(), GEP(Idx), Obj);
}

void CCompositeObj::setValue(CompositeField const& Field, py::handle Obj)
{
  void* Ptr = getFieldData(Field);
  TypeDispatcher<ValueSetter>::switch_(Field.getType(), Ptr, Obj);
}

py::object CCompositeObj::getValue(CompositeField const& Field)
{
  void* Ptr = getFieldData(Field);
  return TypeDispatcher<ValueGetter>::switch_(Field.getType(), Ptr);
}

std::unique_ptr<CObj> CPointerObj::getObj() {
  return TypeDispatcher<PtrToObjView>::switch_(getPointeeType(), getPtr());
}

py::memoryview CPointerObj::getMemoryView(size_t Len)
{
  auto* PointeeTy = getPointeeType();
  const size_t PointeeSize = PointeeTy->getSize();
  // TODO: check integer overflow
  // TODO: ssize_t is an issue
  return py::memoryview{py::buffer_info{getPtr(), static_cast<ssize_t>(PointeeSize), getFormatDescriptor(PointeeTy), static_cast<ssize_t>(PointeeSize*Len)}};
}

py::memoryview CPointerObj::getMemoryViewCStr()
{
  static constexpr auto CharKind = BasicType::getKind<char>();
  auto* PteeType = getPointeeType();
  if (!isa<BasicType>(PteeType) || static_cast<BasicType const*>(PteeType)->getBasicKind() != CharKind) {
    throw TypeError{"pointer must be a pointer to char*!"};
  }
  const size_t Len = strlen((const char*)getPtr());
  return getMemoryView(Len);
}

py::object CFunction::call(py::args const& Args) const
{
  ConvertArgsSwitch::ObjsHolder Holders;
  ConvertArgsSwitch::PyObjsHolder PyHolders;

  std::vector<void*> Ptrs;
  const auto Len = py::len(Args);
  Ptrs.reserve(Len);

  FunctionType const* FTy = getType();
  assert(Len == FTy->getParams().size());

  size_t I = 0;
  for (auto& A: Args) {
    QualType ATy = FTy->getParams()[I];
    auto* AObj = ConvertArgs::switch_(ATy, Holders, PyHolders, A);
    Ptrs.push_back(AObj->dataPtr());
    ++I;
  }

  auto* RetTy = FTy->getReturnType();
  std::unique_ptr<CObj> RetObj;
  if (RetTy) {
    RetObj = CreateObj::switch_(RetTy);
  }
  NF_.call(RetTy ? RetObj->dataPtr() : nullptr, &Ptrs[0]);
  if (RetObj) {
    return py::cast(RetObj.release(), py::return_value_policy::take_ownership);
  }
  return py::none();
}

// Cast
std::unique_ptr<CObj> CPointerObj::cast(Type const* To) const
{
  CObj* Ret = nullptr;
  if (auto const* BTy = dyn_cast<BasicType>(To)) {
    if (BTy->getSize() == sizeof(uintptr_t)) {
      Ret = new CBasicObj<uintptr_t>{*BTy, Data<uintptr_t>::emplace_owned(reinterpret_cast<uintptr_t>(getPtr()))};\
    }
  }
  else
  if (auto const* PTy = dyn_cast<PointerType>(To)) {
    Ret = new CPointerObj{*PTy, Data<void*>::emplace_owned(getPtr())};
  }
  return std::unique_ptr<CObj>{Ret};
}

std::unique_ptr<CObj> CArrayObj::cast(Type const* To) const
{
  CObj* Ret = nullptr;
  if (auto* ATy = dyn_cast<ArrayType>(To)) {
    auto* Ptr = getData();
    if (ATy->getSize() == getType()->getSize() && (((uintptr_t)Ptr) % ATy->getAlign() == 0)) {
      Ret = new CArrayObj{*ATy, Data<void>::view((void*)getData())};
    }
  }
  else
  if (auto* PTy = dyn_cast<PointerType>(To)) {
    Ret = new CPointerObj{*PTy, Data<void*>::emplace_owned((void*)getData())};
  }
  return std::unique_ptr<CObj>{Ret};
}

std::unique_ptr<CObj> CCompositeObj::cast(Type const* To) const
{
  CObj* Ret = nullptr;
  if (auto* PTy = dyn_cast<PointerType>(To)) {
    Ret = new CPointerObj{*PTy, Data<void*>::emplace_owned((void*)getData())};
  }
  else
  if (auto* CTy = dyn_cast<CompositeType>(To)) {
    auto* Ptr = getData();
    if (CTy->getSize() == getType()->getSize() && (((uintptr_t)Ptr) % CTy->getAlign() == 0)) {
      if (auto* STy = dyn_cast<StructType>(To))
        Ret = new CStructObj{*STy, Data<void>::view((void*)Ptr)};
      else
        Ret = new CUnionObj{*dffi::cast<UnionType>(CTy), Data<void>::view((void*)Ptr)};
    }
  }
  return std::unique_ptr<CObj>{Ret};
}
