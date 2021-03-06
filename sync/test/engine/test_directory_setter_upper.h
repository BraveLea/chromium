// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A handy class that takes care of setting up and destroying a
// syncable::Directory instance for unit tests that require one.
//
// The expected usage is to make this a component of your test fixture:
//
//   class AwesomenessTest : public testing::Test {
//    public:
//     virtual void SetUp() {
//       metadb_.SetUp();
//     }
//     virtual void TearDown() {
//       metadb_.TearDown();
//     }
//    protected:
//     TestDirectorySetterUpper metadb_;
//   };
//
// Then, in your tests, get at the directory like so:
//
//   TEST_F(AwesomenessTest, IsMaximal) {
//     ... now use metadb_.directory() to get at syncable::Entry objects ...
//   }
//

#ifndef SYNC_TEST_ENGINE_TEST_DIRECTORY_SETTER_UPPER_H_
#define SYNC_TEST_ENGINE_TEST_DIRECTORY_SETTER_UPPER_H_
#pragma once

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/scoped_temp_dir.h"
#include "sync/util/test_unrecoverable_error_handler.h"
#include "sync/syncable/syncable.h"
#include "sync/test/fake_encryptor.h"
#include "sync/test/null_directory_change_delegate.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace browser_sync {

class TestDirectorySetterUpper {
 public:
  TestDirectorySetterUpper();
  virtual ~TestDirectorySetterUpper();

  // Create a Directory instance open it.
  virtual void SetUp();

  // Undo everything done by SetUp(): close the directory and delete the
  // backing files. Before closing the directory, this will run the directory
  // invariant checks and perform the SaveChanges action on the directory.
  virtual void TearDown();

  syncable::Directory* directory() { return directory_.get(); }

 protected:
  syncable::NullDirectoryChangeDelegate delegate_;
  TestUnrecoverableErrorHandler handler_;

 private:
  void RunInvariantCheck();

  ScopedTempDir temp_dir_;
  FakeEncryptor encryptor_;
  scoped_ptr<syncable::Directory> directory_;
  std::string name_;

  DISALLOW_COPY_AND_ASSIGN(TestDirectorySetterUpper);
};

}  // namespace browser_sync

#endif  // SYNC_TEST_ENGINE_TEST_DIRECTORY_SETTER_UPPER_H_
