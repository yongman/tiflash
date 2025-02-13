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

use lazy_static::lazy_static;

use tantivy::tokenizer::TokenizerManager;

lazy_static! {
    /// Shared for both indexed search and on-demand search.
    pub static ref DEFAULT_TOKENIZERS: TokenizerManager = default_tokenizers();

    /// Shared for both indexed search and on-demand search.
    pub static ref DEFAULT_TOKENIZER: &'static str = "default";
}

fn default_tokenizers() -> TokenizerManager {
    // TODO: Currently we don't register new tokenizers. Only use the default one.
    // But we will add more tokenizers in the future. Examples:
    // - Use https://github.com/testuj-to/tantivy-stemmers for better stemming.
    // - Add Chinese tokenizer.
    // - Add Stopwords filter.
    // - (add more here when ideas come up)
    TokenizerManager::default()
}
