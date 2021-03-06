// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_UDF_UDF_H
#define IMPALA_UDF_UDF_H

#include <boost/cstdint.hpp>
#include <string.h>

// This is the only Impala header required to develop UDFs and UDAs. This header
// contains the types that need to be used and the FunctionContext object. The context
// object serves as the interface object between the UDF/UDA and the impala process.
namespace impala {
  class FunctionContextImpl;
}

namespace impala_udf {

// All input and output values will be one of the structs below. The struct is a simple
// object containing a boolean to store if the value is NULL and the value itself. The
// value is unspecified if the NULL boolean is set.
struct AnyVal;
struct BooleanVal;
struct TinyIntVal;
struct SmallIntVal;
struct IntVal;
struct BigIntVal;
struct StringVal;
struct TimestampVal;

// The FunctionContext is passed to every UDF/UDA and is the interface for the UDF to the
// rest of the system. It contains APIs to examine the system state, report errors
// and manage memory.
class FunctionContext {
 public:
  enum ImpalaVersion {
    v1_2,
  };

  enum Type {
    INVALID_TYPE = 0,
    TYPE_NULL,
    TYPE_BOOLEAN,
    TYPE_TINYINT,
    TYPE_SMALLINT,
    TYPE_INT,
    TYPE_BIGINT,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_TIMESTAMP,
    TYPE_STRING,
    TYPE_FIXED_BUFFER,
  };

  // Returns the version of Impala that's currently running.
  ImpalaVersion version() const;

  // Sets an error for this UDF. If this is called, this will trigger the
  // query to fail.
  void SetError(const char* error_msg);

  // Adds a warning that is returned to the user. This can include things like
  // overflow or other recoverable error conditions.
  // Warnings are capped at a maximum number. Returns true if the warning was
  // added and false if it was ignored due to the cap.
  bool AddWarning(const char* warning_msg);

  // Returns true if there's been an error set.
  bool has_error() const;

  // Returns the current error message. Returns NULL if there is no error.
  const char* error_msg() const;

  // Allocates memory for UDAs. All UDA allocations should use this if possible instead of
  // malloc/new. The UDA is responsible for calling Free() on all buffers returned
  // by Allocate().
  // If this Allocate causes the memory limit to be exceeded, the error will be set
  // in this object causing the query to fail.
  uint8_t* Allocate(int byte_size);

  // Reallocates 'ptr' to the new byte_size. If the currently underlying allocation
  // is big enough, the original ptr will be returned. If the allocation needs to
  // grow, a new allocation is made that is at least 'byte_size' and the contents
  // of 'ptr' will be copied into it.
  // This should be used for buffers that constantly get appended to.
  uint8_t* Reallocate(uint8_t* ptr, int byte_size);

  // Frees a buffer returned from Allocate() or Reallocate()
  void Free(uint8_t* buffer);

  // For allocations that cannot use the Allocate() API provided by this
  // object, TrackAllocation()/Free() can be used to just keep count of the
  // byte sizes. For each call to TrackAllocation(), the UDF/UDA must call
  // the corresponding Free().
  void TrackAllocation(int64_t byte_size);
  void Free(int64_t byte_size);

  // TODO: Do we need to add arbitrary key/value metadata. This would be plumbed
  // through the query. E.g. "select UDA(col, 'sample=true') from tbl".
  // const char* GetMetadata(const char*) const;

  // TODO: Add mechanism for UDAs to update stats similar to runtime profile counters

  // TODO: Add mechanism to query for table/column stats

  // Returns the underlying opaque implementation object. The UDF/UDA should not
  // use this. This is used internally.
  impala::FunctionContextImpl* impl() { return impl_; }

  // Create a test FunctionContext object. The caller is responsible for calling delete
  // on it. This context has additional debugging validation enabled.
  static FunctionContext* CreateTestContext();

  ~FunctionContext();

 private:
  friend class impala::FunctionContextImpl;
  FunctionContext();

  // Disable copy ctor and assignment operator
  FunctionContext(const FunctionContext& other);
  FunctionContext& operator=(const FunctionContext& other);

  impala::FunctionContextImpl* impl_; // Owned by this object.
};

//----------------------------------------------------------------------------
//------------------------------- UDFs ---------------------------------------
//----------------------------------------------------------------------------
// The UDF function must implement this function prototype. This is not
// a typedef as the actual UDF's signature varies from UDF to UDF.
//    typedef <*Val> Evaluate(FunctionContext* context, <const Val& arg>);
//
// The UDF must return one of the *Val structs. The UDF must accept a pointer
// to a FunctionContext object and then a const reference for each of the input arguments.
// NULL input arguments will have NULL passed in.
// Examples of valid Udf signatures are:
//  1) DoubleVal Example1(FunctionContext* context);
//  2) IntVal Example2(FunctionContext* context, const IntVal& a1, const DoubleVal& a2);
//
// UDFs can be variadic. The variable arguments must all come at the end and must be
// the same type. A example signature is:
//  StringVal Concat(FunctionContext* context, const StringVal& separator,
//    int num_var_args, const StringVal* args);
// In this case args[0] is the first variable argument and args[num_var_args - 1] is
// the last.
//
// The UDF should not maintain any state across calls since there is no guarantee
// on how the execution is multithreaded or distributed. Conceptually, the UDF
// should only read the input arguments and return the result, using only the
// FunctionContext as an external object.
//
// Memory Managment: the UDF can assume that memory from input arguments will have
// the same lifetime as results for the UDF. In other words, the UDF can return
// memory from input arguments without making copies. For example, a function like
// substring will not need to allocate and copy the smaller string. For cases where
// the UDF needs a buffer, it should use the StringValue(FunctionContext, len) c'tor.
//
// The UDF can optionally specify a Prepare function. The prepare function is called
// once before any calls to the Udf to evaluate values. This is the appropriate time for
// the Udf to validate versions and things like that.
// If there is an error, this function should call FunctionContext::SetError()/
// FunctionContext::AddWarning().
typedef void (*UdfPrepareFn)(FunctionContext* context);

//----------------------------------------------------------------------------
//------------------------------- UDAs ---------------------------------------
//----------------------------------------------------------------------------
// The UDA execution is broken up into a few steps. The general calling pattern
// is one of these:
//  1) Init(), Evaluate() (repeatedly), Serialize()
//  2) Init(), Merge() (repeatedly), Serialize()
//  3) Init(), Finalize()
// The UDA is registered with three types: the result type, the input type and
// the intermediate type.
//
// If the UDA needs a fixed byte width intermediate buffer, the type should be
// TYPE_FIXED_BUFFER and Impala will allocate the buffer. If the UDA needs an unknown
// sized buffer, it should use TYPE_STRING and allocate it from the FunctionContext
// manually.
// For UDAs that need a complex data structure as the intermediate state, the
// intermediate type should be string and the UDA can cast the ptr to the structure
// it is using.
//
// Memory Management: For allocations that are not returned to Impala, the UDA
// should use the FunctionContext::Allocate()/Free() methods. For StringVal allocations
// returned to Impala (e.g. UdaSerialize()), the UDA should allocate the result
// via StringVal(FunctionContext*, int) ctor and Impala will automatically handle
// freeing it.
//
// For clarity in documenting the UDA interface, the various types will be typedefed
// here. The actual execution resolves all the types at runtime and none of these types
// should actually be used.
typedef AnyVal InputType;
typedef AnyVal InputType2;
typedef AnyVal ResultType;
typedef AnyVal IntermediateType;

// UdaInit is called once for each aggregate group before calls to any of the
// other functions below.
typedef void (*UdaInit)(FunctionContext* context, IntermediateType* result);

// This is called for each input value. The UDA should update result based on the
// input value. The update function can take any number of input arguments. Here
// are some examples:
typedef void (*UdaUpdate)(FunctionContext* context, const InputType& input,
    IntermediateType* result);
typedef void (*UdaUpdate2)(FunctionContext* context, const InputType& input,
    const InputType2& input2, IntermediateType* result);

// Merge an intermediate result 'src' into 'dst'.
typedef void (*UdaMerge)(FunctionContext* context, const IntermediateType& src,
    IntermediateType* dst);

// Serialize the intermediate type. The serialized data is then sent across the
// wire. This is not called unless the intermediate type is String.
// No additional functions will be called with this FunctionContext object and the
// UDA should do final clean (e.g. Free()) here.
typedef const IntermediateType (*UdaSerialize)(FunctionContext* context,
    const IntermediateType& type);

// Called once at the end to return the final value for this UDA.
// No additional functions will be called with this FunctionContext object and the
// UDA should do final clean (e.g. Free()) here.
typedef ResultType (*UdaFinalize)(FunctionContext* context, const IntermediateType& v);

//----------------------------------------------------------------------------
//-------------Implementation of the *Val structs ----------------------------
//----------------------------------------------------------------------------
struct AnyVal {
  bool is_null;
  AnyVal(bool is_null = false) : is_null(is_null) {}
};

struct BooleanVal : public AnyVal {
  bool val;

  BooleanVal(bool val = false) : val(val) {}

  static BooleanVal null() {
    BooleanVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const BooleanVal& other) const {
    if (is_null && other.is_null) return true;
    if (is_null || other.is_null) return false;
    return val == other.val;
  }
  bool operator!=(const BooleanVal& other) const { return !(*this == other); }
};

struct TinyIntVal : public AnyVal {
  int8_t val;

  TinyIntVal(int8_t val = 0) : val(val) { }

  static TinyIntVal null() {
    TinyIntVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const TinyIntVal& other) const {
    if (is_null && other.is_null) return true;
    if (is_null || other.is_null) return false;
    return val == other.val;
  }
  bool operator!=(const TinyIntVal& other) const { return !(*this == other); }
};

struct SmallIntVal : public AnyVal {
  int16_t val;

  SmallIntVal(int16_t val = 0) : val(val) { }

  static SmallIntVal null() {
    SmallIntVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const SmallIntVal& other) const {
    if (is_null && other.is_null) return true;
    if (is_null || other.is_null) return false;
    return val == other.val;
  }
  bool operator!=(const SmallIntVal& other) const { return !(*this == other); }
};

struct IntVal : public AnyVal {
  int32_t val;

  IntVal(int32_t val = 0) : val(val) { }

  static IntVal null() {
    IntVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const IntVal& other) const {
    if (is_null && other.is_null) return true;
    if (is_null || other.is_null) return false;
    return val == other.val;
  }
  bool operator!=(const IntVal& other) const { return !(*this == other); }
};

struct BigIntVal : public AnyVal {
  int64_t val;

  BigIntVal(int64_t val = 0) : val(val) { }

  static BigIntVal null() {
    BigIntVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const BigIntVal& other) const {
    if (is_null && other.is_null) return true;
    if (is_null || other.is_null) return false;
    return val == other.val;
  }
  bool operator!=(const BigIntVal& other) const { return !(*this == other); }
};

struct FloatVal : public AnyVal {
  float val;

  FloatVal(float val = 0) : val(val) { }

  static FloatVal null() {
    FloatVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const FloatVal& other) const {
    return is_null == other.is_null && val == other.val;
  }
  bool operator!=(const FloatVal& other) const { return !(*this == other); }
};

struct DoubleVal : public AnyVal {
  double val;

  DoubleVal(double val = 0) : val(val) { }

  static DoubleVal null() {
    DoubleVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const DoubleVal& other) const {
    if (is_null && other.is_null) return true;
    if (is_null || other.is_null) return false;
    return val == other.val;
  }
  bool operator!=(const DoubleVal& other) const { return !(*this == other); }
};

// This object has a compatible storage format with boost::ptime.
struct TimestampVal : public AnyVal {
  // Gregorian date. This has the same binary format as boost::gregorian::date.
  int32_t date;
  // Nanoseconds in current day.
  int64_t time_of_day;

  TimestampVal(int32_t date = 0, int64_t time_of_day = 0) :
    date(date), time_of_day(time_of_day) {
  }

  static TimestampVal null() {
    TimestampVal result;
    result.is_null = true;
    return result;
  }

  bool operator==(const TimestampVal& other) const {
    if (is_null && other.is_null) return true;
    if (is_null || other.is_null) return false;
    return date == other.date && time_of_day == other.time_of_day;
  }
  bool operator!=(const TimestampVal& other) const { return !(*this == other); }
};

// Note: there is a difference between a NULL string (is_null == true) and an
// empty string (len == 0).
struct StringVal : public AnyVal {
  int len;
  uint8_t* ptr;

  // Construct a StringVal from ptr/len. Note: this does not make a copy of ptr
  // so the buffer must exist as long as this StringVal does.
  StringVal(uint8_t* ptr = NULL, int len = 0) : len(len), ptr(ptr) {}

  // Construct a StringVal from NULL-terminated c-string. Note: this does not make a
  // copy of ptr so the underlying string must exist as long as this StringVal does.
  StringVal(const char* ptr) : len(strlen(ptr)), ptr((uint8_t*)ptr) {}

  static StringVal null() {
    StringVal sv;
    sv.is_null = true;
    return sv;
  }

  // Creates a StringVal, allocating a new buffer with 'len'. This should
  // be used to return StringVal objects in UDF/UDAs that need to allocate new
  // string memory.
  StringVal(FunctionContext* context, int len);

  bool operator==(const StringVal& other) const {
    if (is_null != other.is_null) return false;
    if (is_null) return true;
    if (len != other.len) return false;
    return ptr == other.ptr || memcmp(ptr, other.ptr, len) == 0;
  }
  bool operator!=(const StringVal& other) const { return !(*this == other); }
};

typedef uint8_t* BufferVal;

}

#endif
