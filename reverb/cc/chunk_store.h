// Copyright 2019 DeepMind Technologies Limited.
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

#ifndef REVERB_CC_CHUNK_STORE_H_
#define REVERB_CC_CHUNK_STORE_H_

#include <memory>
#include <utility>
#include <vector>

#include <cstdint>
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "reverb/cc/platform/thread.h"
#include "reverb/cc/schema.pb.h"
#include "reverb/cc/support/queue.h"
#include "tensorflow/core/lib/core/status.h"

namespace deepmind {
namespace reverb {

// Maintains a bijection from chunk keys to Chunks. For inserting, the caller
// passes ChunkData which contains a chunk key and the actual data. We use the
// key for the mapping and wrap the ChunkData with in a thin class which
// provides a read-only accessor to the ChunkData.
//
// For ChunkData that populates sequence_range, an interval tree is maintained
// to allow lookups of intersecting ranges.
//
// Each Chunk is reference counted individually. When its reference count drops
// to zero, the Chunk is destroyed and subsequent calls to Get() will no longer
// return that Chunk. Please note that this container only holds a weak pointer
// to a Chunk, and thus does not count towards the reference count. For this
// reason, Insert() returns a shared pointer, as otherwise the Chunk would be
// destroyed right away.
//
// All public methods are thread safe.
class ChunkStore {
 public:
  using Key = uint64_t;

  class Chunk {
   public:
    explicit Chunk(ChunkData data) : data_(std::move(data)) {}

    // Returns the proto data of the chunk.
    const ChunkData& data() const { return data_; }

    // (Potentially cached) size of `data`.
    size_t DataByteSizeLong() const {
      if (data_byte_size_ == 0) {
        data_byte_size_ = data_.ByteSizeLong();
      }
      return data_byte_size_;
    }

   private:
    ChunkData data_;
    mutable size_t data_byte_size_ = 0;
  };

  // Starts `cleaner_`. `cleanp_batch_size` is the number of keys the cleaner
  // should wait for before acquiring the lock and erasing them from `data_`.
  explicit ChunkStore(int cleanup_batch_size = 1000);

  // Stops `cleaner_` closes `delete_keys_`.
  ~ChunkStore();

  // Attempts to insert a Chunk into the map using the key inside `item`. If no
  // entry existed for the key, a new Chunk is created, inserted and returned.
  // Otherwise, the existing chunk is returned.
  std::shared_ptr<Chunk> Insert(ChunkData item) ABSL_LOCKS_EXCLUDED(mu_);

  // Gets the Chunk for each given key. Returns an error if one of the items
  // does not exist or if `Close` has been called. On success, the returned
  // items are in the same order as given in `keys`.
  tensorflow::Status Get(absl::Span<const Key> keys,
                         std::vector<std::shared_ptr<Chunk>> *chunks)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Blocks until `num_chunks` expired entries have been cleaned up from
  // `data_`. This method is called automatically by a background thread to
  // limit memory size, but does not have any effect on the semantics of Get()
  // or Insert() calls.
  //
  // Returns false if `delete_keys_` closed before `num_chunks` could be popped.
  bool CleanupInternal(int num_chunks) ABSL_LOCKS_EXCLUDED(mu_);

 private:
  // Gets an item. Returns nullptr if the item does not exist.
  std::shared_ptr<Chunk> GetItem(Key key) ABSL_SHARED_LOCKS_REQUIRED(mu_);

  // Holds the actual mapping of key to Chunk. We only hold a weak pointer to
  // the Chunk, which means that destruction and reference counting of the
  // chunks happens independently of this map.
  absl::flat_hash_map<Key, std::weak_ptr<Chunk>> data_ ABSL_GUARDED_BY(mu_);

  // Mutex protecting access to `data_`.
  mutable absl::Mutex mu_;

  // Queue of keys of deleted items that will be cleaned up by `cleaner_`. Note
  // the queue have to be allocated on the heap in order to avoid dereferncing
  // errors caused by a stack allocated ChunkStore getting destroyed before all
  // Chunk have been destroyed.
  std::shared_ptr<internal::Queue<Key>> delete_keys_;

  // Consumes `delete_keys_` to remove dead pointers in `data_`.
  std::unique_ptr<internal::Thread> cleaner_;
};

}  // namespace reverb
}  // namespace deepmind

#endif  // REVERB_CC_CHUNK_STORE_H_
