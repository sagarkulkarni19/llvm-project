//===-- StackFrameRecognizer.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_TARGET_STACKFRAMERECOGNIZER_H
#define LLDB_TARGET_STACKFRAMERECOGNIZER_H

#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Utility/StructuredData.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/ValueObject/ValueObjectList.h"
#include "lldb/lldb-private-forward.h"
#include "lldb/lldb-public.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace lldb_private {

/// \class RecognizedStackFrame
///
/// This class provides extra information about a stack frame that was
/// provided by a specific stack frame recognizer. Right now, this class only
/// holds recognized arguments (via GetRecognizedArguments).
class RecognizedStackFrame
    : public std::enable_shared_from_this<RecognizedStackFrame> {
public:
  virtual ~RecognizedStackFrame() = default;

  virtual lldb::ValueObjectListSP GetRecognizedArguments() {
    return m_arguments;
  }
  virtual lldb::ValueObjectSP GetExceptionObject() {
    return lldb::ValueObjectSP();
  }
  virtual lldb::StackFrameSP GetMostRelevantFrame() { return nullptr; }

  std::string GetStopDescription() { return m_stop_desc; }
  /// Controls whether this frame should be filtered out when
  /// displaying backtraces, for example.
  virtual bool ShouldHide() { return false; }

protected:
  lldb::ValueObjectListSP m_arguments;
  std::string m_stop_desc;
};

/// \class StackFrameRecognizer
///
/// A base class for frame recognizers. Subclasses (actual frame recognizers)
/// should implement RecognizeFrame to provide a RecognizedStackFrame for a
/// given stack frame.
class StackFrameRecognizer
    : public std::enable_shared_from_this<StackFrameRecognizer> {
public:
  virtual lldb::RecognizedStackFrameSP RecognizeFrame(
      lldb::StackFrameSP frame) {
    return lldb::RecognizedStackFrameSP();
  };
  virtual std::string GetName() {
    return "";
  }

  virtual ~StackFrameRecognizer() = default;
};

/// \class ScriptedStackFrameRecognizer
///
/// Python implementation for frame recognizers. An instance of this class
/// tracks a particular Python classobject, which will be asked to recognize
/// stack frames.
class ScriptedStackFrameRecognizer : public StackFrameRecognizer {
  lldb_private::ScriptInterpreter *m_interpreter;
  lldb_private::StructuredData::ObjectSP m_python_object_sp;

  std::string m_python_class;

public:
  ScriptedStackFrameRecognizer(lldb_private::ScriptInterpreter *interpreter,
                               const char *pclass);
  ~ScriptedStackFrameRecognizer() override = default;

  std::string GetName() override {
    return GetPythonClassName();
  }

  const char *GetPythonClassName() { return m_python_class.c_str(); }

  lldb::RecognizedStackFrameSP RecognizeFrame(
      lldb::StackFrameSP frame) override;

private:
  ScriptedStackFrameRecognizer(const ScriptedStackFrameRecognizer &) = delete;
  const ScriptedStackFrameRecognizer &
  operator=(const ScriptedStackFrameRecognizer &) = delete;
};

/// Class that provides a registry of known stack frame recognizers.
class StackFrameRecognizerManager {
public:
  /// Add a new recognizer that triggers on a given symbol name.
  ///
  /// \param symbol_mangling controls whether the symbol name should be
  /// compared to the mangled or demangled name.
  void AddRecognizer(lldb::StackFrameRecognizerSP recognizer,
                     ConstString module, llvm::ArrayRef<ConstString> symbols,
                     Mangled::NamePreference symbol_mangling,
                     bool first_instruction_only = true);

  /// Add a new recognizer that triggers on a symbol regex.
  ///
  /// \param symbol_mangling controls whether the regex should apply
  /// to the mangled or demangled name.
  void AddRecognizer(lldb::StackFrameRecognizerSP recognizer,
                     lldb::RegularExpressionSP module,
                     lldb::RegularExpressionSP symbol,
                     Mangled::NamePreference symbol_mangling,
                     bool first_instruction_only = true);

  void
  ForEach(std::function<void(uint32_t recognizer_id, bool enabled,
                             std::string recognizer_name, std::string module,
                             llvm::ArrayRef<ConstString> symbols,
                             Mangled::NamePreference name_preference,
                             bool regexp)> const &callback);

  bool SetEnabledForID(uint32_t recognizer_id, bool enabled);
  bool RemoveRecognizerWithID(uint32_t recognizer_id);

  void RemoveAllRecognizers();

  lldb::StackFrameRecognizerSP GetRecognizerForFrame(lldb::StackFrameSP frame);

  lldb::RecognizedStackFrameSP RecognizeFrame(lldb::StackFrameSP frame);
  /// Returns a number that changes whenever the list of recognizers
  /// has been modified.
  uint16_t GetGeneration() const { return m_generation; }

private:
  /// Increase the generation counter.
  void BumpGeneration();

  struct RegisteredEntry {
    uint32_t recognizer_id;
    lldb::StackFrameRecognizerSP recognizer;
    bool is_regexp;
    ConstString module;
    lldb::RegularExpressionSP module_regexp;
    std::vector<ConstString> symbols;
    lldb::RegularExpressionSP symbol_regexp;
    Mangled::NamePreference symbol_mangling;
    bool first_instruction_only;
    bool enabled;
  };

  std::deque<RegisteredEntry> m_recognizers;
  uint16_t m_generation = 0;
};

/// \class ValueObjectRecognizerSynthesizedValue
///
/// ValueObject subclass that presents the passed ValueObject as a recognized
/// value with the specified ValueType. Frame recognizers should return
/// instances of this class as the returned objects in GetRecognizedArguments().
class ValueObjectRecognizerSynthesizedValue : public ValueObject {
 public:
  static lldb::ValueObjectSP Create(ValueObject &parent, lldb::ValueType type) {
    return (new ValueObjectRecognizerSynthesizedValue(parent, type))->GetSP();
  }
  ValueObjectRecognizerSynthesizedValue(ValueObject &parent,
                                        lldb::ValueType type)
      : ValueObject(parent), m_type(type) {
    SetName(parent.GetName());
  }

  llvm::Expected<uint64_t> GetByteSize() override {
    return m_parent->GetByteSize();
  }
  lldb::ValueType GetValueType() const override { return m_type; }
  bool UpdateValue() override {
    if (!m_parent->UpdateValueIfNeeded()) return false;
    m_value = m_parent->GetValue();
    return true;
  }
  llvm::Expected<uint32_t>
  CalculateNumChildren(uint32_t max = UINT32_MAX) override {
    return m_parent->GetNumChildren(max);
  }
  CompilerType GetCompilerTypeImpl() override {
    return m_parent->GetCompilerType();
  }
  bool IsSynthetic() override { return true; }

 private:
  lldb::ValueType m_type;
};

} // namespace lldb_private

#endif // LLDB_TARGET_STACKFRAMERECOGNIZER_H
