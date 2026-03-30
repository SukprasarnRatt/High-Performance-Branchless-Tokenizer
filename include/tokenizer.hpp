#ifndef TOKENIZER_HPP
#define TOKENIZER_HPP

#include <vector>
#include <cstddef>

void loadDictionaryAlphaNumeric(char lookUpTable[256]);

std::vector<char*> tokenize(char* buffer, std::size_t bufferSize, char lookUpTable[256]);

#endif