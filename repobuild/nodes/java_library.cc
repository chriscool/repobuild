// Copyright 2013
// Author: Christopher Van Arsdale

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include "common/log/log.h"
#include "common/strings/path.h"
#include "common/strings/strutil.h"
#include "repobuild/env/input.h"
#include "repobuild/nodes/java_library.h"
#include "repobuild/nodes/util.h"
#include "repobuild/reader/buildfile.h"

using std::vector;
using std::string;
using std::set;

namespace repobuild {

JavaLibraryNode::JavaLibraryNode(const TargetInfo& t,
                                 const Input& i,
                                 DistSource* s)
    : Node(t, i, s) {
}

JavaLibraryNode::~JavaLibraryNode() {}

void JavaLibraryNode::Parse(BuildFile* file, const BuildFileNode& input) {
  Node::Parse(file, input);

  // java_sources
  current_reader()->ParseRepeatedFiles("java_sources", &sources_);

  ParseInternal(file, input);
}

void JavaLibraryNode::Set(BuildFile* file,
                          const BuildFileNode& input,
                          const vector<Resource>& sources) {
  Node::Parse(file, input);
  sources_ = sources;
  ParseInternal(file, input);
}

void JavaLibraryNode::ParseInternal(BuildFile* file,
                                    const BuildFileNode& input) {
  // root dir for output class files, which is also a class path down below,
  // see Init().
  string java_root = current_reader()->ParseSingleDirectory("java_root");
  component_.reset(new ComponentHelper("", java_root));

  // classpath info.
  vector<Resource> java_classpath_dirs;
  current_reader()->ParseRepeatedFiles("java_additional_classpaths",
                                       false,  // directory need not exist.
                                       &java_classpath_dirs);
  for (const Resource& r : java_classpath_dirs) {
    java_classpath_.push_back(r.path());
  }
  java_classpath_.push_back(java_root);
  java_classpath_.push_back(strings::JoinPath(Node::input().genfile_dir(),
                                              StripSpecialDirs(java_root)));
  java_classpath_.push_back(strings::JoinPath(Node::input().object_dir(),
                                              StripSpecialDirs(java_root)));
  std::sort(java_classpath_.begin(), java_classpath_.end(),
            [](const string& a, const string& b) -> bool {
              return a.size() > b.size();
            });

  // javac args
  current_reader()->ParseRepeatedString("java_local_compile_args",
                                        &java_local_compile_args_);
  current_reader()->ParseRepeatedString("java_compile_args",  // inherited.
                                        &java_compile_args_);

  // jar args
  current_reader()->ParseRepeatedString("java_jar_args",
                                        &java_jar_args_);

  // Sanity checks.
  for (const Resource& source : sources_) {
    CHECK(strings::HasSuffix(source.path(), ".java"))
        << "Invalid java source "
        << source << " in target " << target().full_path();
  }
}

void JavaLibraryNode::LocalWriteMakeInternal(bool write_user_target,
                                             Makefile* out) const {
  // Figure out the set of input files.
  ResourceFileSet input_files;
  InputDependencyFiles(JAVA, &input_files);

  // Compile all .java files at the same time, for efficiency.
  WriteCompile(input_files, out);

  // Now write user target (so users can type "make path/to/exec|lib").
  if (write_user_target) {
    ResourceFileSet targets;
    for (const Resource& source : sources_) {
      targets.Add(ClassFile(source));
    }
    WriteBaseUserTarget(targets, out);
  }
}

void JavaLibraryNode::WriteCompile(const ResourceFileSet& input_files,
                                   Makefile* out) const {
  vector<Resource> obj_files;
  set<string> directories;
  for (const Resource& source : sources_) {
    obj_files.push_back(ClassFile(source));
    directories.insert(obj_files.back().dirname());
  }

  // NB: Make has a bug with multiple output files and parallel execution.
  // Thus, we use a touchfile and generate a separate rule for each output file.
  Resource touchfile = Touchfile("compile");
  Makefile::Rule* rule = out->StartRule(
      touchfile.path(),
      strings::JoinWith(" ",
                        strings::JoinAll(input_files.files(), " "),
                        strings::JoinAll(sources_, " ")));

  // Mkdir commands.
  for (const string d : directories) {
    rule->WriteCommand("mkdir -p " + d);
  }

  // Compile command.
  string compile = "javac";

  // Collect class paths.
  set<string> java_classpath;
  IncludeDirs(JAVA, &java_classpath);

  // class path.
  string include_dirs = strings::JoinWith(
      " ",
      "-cp ",
      strings::JoinWith(":",
                        input().root_dir(),
                        input().genfile_dir(),
                        input().source_dir(),
                        strings::JoinPath(input().source_dir(),
                                          input().genfile_dir()),
                        strings::JoinAll(java_classpath, ":")));

  // javac compile args.
  set<string> compile_args;
  CompileFlags(JAVA, &compile_args);
  compile_args.insert(java_local_compile_args_.begin(),
                      java_local_compile_args_.end());
  for (const string& f : input().flags("-JC")) {
    compile_args.insert(f);
  }

  rule->WriteUserEcho("Compiling", target().make_path() + " (java)");
  rule->WriteCommand("mkdir -p " + ObjectRoot().path());
  rule->WriteCommand(strings::JoinWith(
      " ",
      compile,
      "-d " + ObjectRoot().path(),
      "-s " + input().genfile_dir(),
      strings::JoinAll(compile_args, " "),
      include_dirs,
      strings::JoinAll(sources_, " ")));
  rule->WriteCommand("mkdir -p " + touchfile.dirname());
  rule->WriteCommand("touch " + touchfile.path());
  out->FinishRule(rule);

  // Secondary rules depend on touchfile and make sure each classfile is in
  // place.
  for (int i = 0; i < obj_files.size(); ++i) {
    const Resource& object_file = obj_files[i];
    CHECK(strings::HasPrefix(object_file.path(), ObjectRoot().path() + "/"));
    string suffix = object_file.path().substr(ObjectRoot().path().size() + 1);
    Makefile::Rule* rule = out->StartRule(object_file.path(), touchfile.path());
    // Make sure we actually generated all of the object files, otherwise the
    // user may have specified the wrong java_out_root.
    rule->WriteCommand("if [ ! -f " + object_file.path() + " ]; then " +
                       "echo \"Class file not generated: "
                       + object_file.path() +
                       ", or it was generated in an unexpected location. Make "
                       "sure java_root is specified correctly or the "
                       "package name for the object is: " +
                       strings::ReplaceAll(suffix, "/", ".") +
                       "\"; exit 1; fi");
    rule->WriteCommand("touch " + object_file.path());
    out->FinishRule(rule);
  }

  // ObjectRoot directory rule
  rule = out->StartRule(RootTouchfile().path(), strings::JoinAll(obj_files, " "));
  rule->WriteCommand("mkdir -p " + RootTouchfile().dirname());
  rule->WriteCommand("touch " + RootTouchfile().path());
  out->FinishRule(rule);
}

void JavaLibraryNode::LocalLinkFlags(LanguageType lang,
                                     std::set<std::string>* flags) const {
  if (lang == JAVA) {
    flags->insert(java_jar_args_.begin(), java_jar_args_.end());
  }
}

void JavaLibraryNode::LocalCompileFlags(LanguageType lang,
                                        std::set<std::string>* flags) const {
  if (lang == JAVA) {
    flags->insert(java_compile_args_.begin(), java_compile_args_.end());
  }
}

void JavaLibraryNode::LocalIncludeDirs(LanguageType lang,
                                       std::set<std::string>* dirs) const {
  dirs->insert(java_classpath_.begin(), java_classpath_.end());
  dirs->insert(ObjectRoot().path());
}

void JavaLibraryNode::LocalObjectFiles(LanguageType lang,
                                       ResourceFileSet* files) const {
  Node::LocalObjectFiles(lang, files);
  for (const Resource& r : sources_) {
    files->Add(ClassFile(r));
  }
}

void JavaLibraryNode::LocalObjectRoots(LanguageType lang,
                                       ResourceFileSet* dirs) const {
  Node::LocalObjectRoots(lang, dirs);
  dirs->Add(RootTouchfile());
}

void JavaLibraryNode::LocalDependencyFiles(LanguageType lang,
                                           ResourceFileSet* files) const {
  for (const Resource& r : sources_) {
    files->Add(r);
  }

  // Java also needs class files for dependent javac invocations.
  LocalObjectFiles(lang, files);
}

Resource JavaLibraryNode::ClassFile(const Resource& source) const {
  CHECK(strings::HasSuffix(source.path(), ".java"));

  // Strip our leading directories.
  string path = StripSpecialDirs(
      source.path().substr(0, source.path().size() - 4)) + "class";
  for (const string& classpath : java_classpath_) {
    if (strings::HasPrefix(path, classpath + "/")) {
      path = path.substr(classpath.size() + 1);
      break;
    }
  }
  path = GetComponentHelper(path)->RewriteFile(input(), path);

  // This file is going under ObjectRoot();
  return Resource::FromLocalPath(ObjectRoot().path(), path);
}

Resource JavaLibraryNode::ObjectRoot() const {
  return Resource::FromLocalPath(input().object_dir(),
                                 "lib_" + target().make_path());
}

Resource JavaLibraryNode::RootTouchfile() const {
  return Resource::FromLocalPath(ObjectRoot().path(), ".dummy.touch");
}

}  // namespace repobuild
