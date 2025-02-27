// Copyright 2025 PingCAP, Inc.
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

use std::collections::HashSet;

use anyhow::{bail, Result};
use tantivy::tokenizer::TextAnalyzer;

pub struct OnDemandSearcher {
    /// Used for both source text and query text.
    /// TODO: Support using different tokenizers for source text and query text.
    text_tokenizer: TextAnalyzer,

    /// Pre-tokenized query.
    query_tokens: HashSet<String>,
}

impl OnDemandSearcher {
    pub fn new(query: &str) -> Result<Self> {
        let tokenizers = crate::defaults::DEFAULT_TOKENIZERS.clone();
        let tokenizer_name = *crate::defaults::DEFAULT_TOKENIZER;
        let tokenizer = tokenizers.get(tokenizer_name);
        if tokenizer.is_none() {
            // We use a result here, because in future tokenizer could be
            // specified by the user.
            bail!("Tokenizer {:?} not found", tokenizer_name);
        }

        // Pre-tokenize the query so that we don't need to tokenize it multiple times
        // when matching against multiple source texts.
        let mut query_tokenizer = tokenizer.clone().unwrap();
        let mut query_tokens = query_tokenizer.token_stream(query);
        let mut ret_query_tokens = HashSet::new();
        while query_tokens.advance() {
            let token = query_tokens.token_mut();
            let text = std::mem::take(&mut token.text);
            ret_query_tokens.insert(text);
        }

        Ok(Self {
            text_tokenizer: tokenizer.unwrap(),
            query_tokens: ret_query_tokens,
        })
    }

    /// Note: This API may change when we care about the rank.
    pub fn is_match(&self, src: &str) -> bool {
        // TODO: Currently this implementation is not compatible
        // with Tantivy's query parser. Tantivy's syntax like +foo -bar
        // does not take any effect.
        // There are two ways,
        // A. Use TermSetQuery for indexed search (reduce its functionality),
        //    align with this OnDemandSearcher.
        // B. Extend this to be compatible with Tantivy's query parser.

        // TODO: Support parsing query into TermSetQuery before searching.
        // This avoids tokenizing the query multiple times.

        let mut src_tokenizer = self.text_tokenizer.clone();
        let mut src_tokens = src_tokenizer.token_stream(src);
        while src_tokens.advance() {
            let token = src_tokens.token();
            if self.query_tokens.contains(&token.text) {
                return true;
            }
        }

        false
    }
}

/// For FFI
fn new_on_demand_searcher(query: &str) -> Result<Box<OnDemandSearcher>> {
    let searcher = OnDemandSearcher::new(query)?;
    Ok(Box::new(searcher))
}

#[cxx::bridge(namespace = "ClaraFTS")]
mod ffi {
    extern "Rust" {
        type OnDemandSearcher;

        fn new_on_demand_searcher(query: &str) -> Result<Box<OnDemandSearcher>>;

        fn is_match(self: &OnDemandSearcher, src: &str) -> bool;
    }
}

#[cfg(test)]
mod tests {

    use super::*;

    fn is_index_match(src: &str, query: &str) -> Result<bool> {
        let mut index_writer = crate::IndexWriterInMemory::new()?;
        index_writer.add_document(src)?;
        let buffer = index_writer.finalize()?;
        let index_reader = crate::IndexReader::new_memory(buffer)?;
        let results = index_reader.search_no_score(query, &crate::BitmapFilter::all_match())?;
        Ok(!results.is_empty())
    }

    fn assert_match_eq(src: &str, query: &str) -> Result<()> {
        let index_match_result = is_index_match(src, query)?;
        let searcher = OnDemandSearcher::new(query)?;
        let noindex_match_result = searcher.is_match(src);
        assert_eq!(
            index_match_result, noindex_match_result,
            "Search `{}` in `{}` got different result: found_by_index={}, found_by_noindex={}",
            query, src, index_match_result, noindex_match_result
        );
        Ok(())
    }

    #[test]
    fn test_search_same_as_index() -> Result<()> {
        // This test verifies that the OnDemandSearcher returns the same result
        // as using the index.

        let src = "Being too popular can be such a hassle";
        let queries = vec![
            "popular",
            "people",
            "peoples",
            "can",
            "being",
            "beING",
            "CAN",
            "furina",
            "eing",
            "foo bar",
            "foo bar be",
            "ass",
            "ass a",
            "   ass    a   ",
            "elssah",
        ];
        for query in queries {
            assert_match_eq(src, query)?;
        }
        Ok(())
    }
}
