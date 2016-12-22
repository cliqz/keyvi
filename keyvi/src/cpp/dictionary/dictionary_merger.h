/* * keyvi - A key value store.
 *
 * Copyright 2015, 2016 Hendrik Muhs<hendrik.muhs@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * dictionary_merger.h
 *
 *  Created on: Feb 27, 2016
 *      Author: hendrik
 */

#ifndef DICTIONARY_MERGER_H_
#define DICTIONARY_MERGER_H_

#include <queue>
#include <memory>

#include "dictionary/fsa/generator_adapter.h"
#include "dictionary/fsa/automata.h"
#include "dictionary/fsa/entry_iterator.h"
#include "dictionary/fsa/internal/constants.h"

//#define ENABLE_TRACING
#include "dictionary/util/trace.h"

namespace keyvi {
namespace dictionary {

typedef const fsa::internal::IValueStoreWriter::vs_param_t merger_param_t;

template<class PersistenceT, class ValueStoreT = fsa::internal::NullValueStore>
class DictionaryMerger
final {
private:
    class SegmentIterator {
        using EntryIteratorPtr = std::shared_ptr<fsa::EntryIterator>;

    public:
        /**
         *
         * @param segment_index, merge segment index also used as a priority indicator
         *                          when comparing two keys with the same value.
         */
        SegmentIterator(const fsa::EntryIterator& e, size_t segment_index) :
                entry_iterator_ptr_(std::make_shared<fsa::EntryIterator>(e)),
                segment_index_(segment_index)
        {}

        bool operator<(const SegmentIterator& rhs) const {
            // very important difference in semantics: we have to ensure that in case of equal key,
            // the iterator with the higher index (priority) is taken

            if (segment_index_ < rhs.segment_index_) {
                return entryIterator() > rhs.entryIterator();
            }

            return rhs.entryIterator() < entryIterator();
        }

        operator bool() const {
            return entryIterator() != endIterator();
        }

        SegmentIterator& operator++() {
            ++(*entry_iterator_ptr_);
            return *this;
        }

        const fsa::EntryIterator& entryIterator() const {
            return *entry_iterator_ptr_;
        }

        const size_t segmentIndex() const {
            return segment_index_;
        }

    private:
        static const fsa::EntryIterator& endIterator() {
            static fsa::EntryIterator end_it;
            return end_it;
        }

    private:
        EntryIteratorPtr entry_iterator_ptr_;
        size_t segment_index_;
    };

public:
    DictionaryMerger(size_t memory_limit = 1073741824, const merger_param_t& params = merger_param_t())
            : dicts_to_merge_(),
              memory_limit_(memory_limit),
              params_(params)
    {
        if (params_.count(TEMPORARY_PATH_KEY) == 0) {
            params_[TEMPORARY_PATH_KEY] = boost::filesystem::temp_directory_path().string();
        }
        if (params_.count(MERGE_MODE) > 0) {
            append_merge_ = MERGE_APPEND == params_[MERGE_MODE];
        }
    }

    void Add(const std::string& filename) {
        inputFiles_.push_back(filename);

        fsa::automata_t fsa;

        if (append_merge_) {
            fsa.reset(new fsa::Automata(filename, loading_strategy_types::lazy, false));
        } else {
            fsa.reset(new fsa::Automata(filename));
        }

        if (fsa->GetValueStoreType() != ValueStoreT::GetValueStoreType()) {
            throw std::invalid_argument("Dictionaries must have the same type.");
        }

        dicts_to_merge_.push_back(fsa);
    }

    /**
     * Set a custom manifest to be embedded into the index file.
     *
     * @param manifest as JSON string
     */
    void SetManifestFromString(const std::string& manifest) {
        manifest_ = manifest;
    }

    void Merge(const std::string& filename) {
        using GeneratorAdapter = fsa::GeneratorAdapterInterface<PersistenceT, ValueStoreT>;

        std::priority_queue<SegmentIterator> pqueue;

        size_t i = 0;
        size_t sparse_array_size_sum = 0;
        for (auto fsa: dicts_to_merge_) {
            fsa::EntryIterator e_it(fsa);
            sparse_array_size_sum += fsa->SparseArraySize();
            pqueue.push(SegmentIterator(e_it, i++));
        }

        ValueStoreT* value_store = append_merge_ ? new ValueStoreT(inputFiles_) : new ValueStoreT(params_);

        auto generator = GeneratorAdapter::CreateGenerator(sparse_array_size_sum, memory_limit_, params_, value_store);

        std::string top_key;

        while (!pqueue.empty()) {
            auto segment_it = pqueue.top();
            pqueue.pop();

            top_key = segment_it.entryIterator().GetKey();

            // check for same keys and merge only the most recent one
            while (!pqueue.empty() and pqueue.top().entryIterator().operator==(top_key)) {

                auto to_inc = pqueue.top();

                pqueue.pop();
                if (++to_inc) {
                    TRACE("push iterator");
                    pqueue.push(to_inc);
                }
            }

            fsa::ValueHandle handle;
            handle.no_minimization = false;

            // get the weight value, for now simple: does not require access to the value store itself
            handle.weight = value_store->GetMergeWeight(segment_it.entryIterator().GetValueId());

            if (append_merge_) {
                handle.value_idx = value_store->GetMergeValueId(segment_it.segmentIndex(),
                                                                segment_it.entryIterator().GetValueId());
            } else {
                handle.value_idx = value_store->GetValue(
                        segment_it.entryIterator().GetFsa()->GetValueStore()->GetValueStorePayload(),
                        segment_it.entryIterator().GetValueId(),
                        handle.no_minimization);
            }

            TRACE("Add key: %s", top_key.c_str());
            generator->Add(std::move(top_key), handle);

            if (++segment_it) {
                pqueue.push(segment_it);
            }
        }

        dicts_to_merge_.clear();

        TRACE("finished iterating, do final compile.");

        generator->CloseFeeding();

        generator->SetManifestFromString(manifest_);
        generator->WriteToFile(filename);
    }

private:
    bool                            append_merge_ = false;
    std::vector<fsa::automata_t>    dicts_to_merge_;
    std::vector<std::string>        inputFiles_;

    size_t memory_limit_;
    fsa::internal::IValueStoreWriter::vs_param_t params_;
    std::string manifest_ = std::string();
};

} /* namespace dictionary */
} /* namespace keyvi */


#endif /* SRC_CPP_DICTIONARY_DICTIONARY_MERGER_H_ */
