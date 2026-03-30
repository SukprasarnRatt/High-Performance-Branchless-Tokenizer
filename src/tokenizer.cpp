#include "tokenizer.hpp"
#include <cctype>

void loadDictionaryAlphaNumeric(char lookUpTable[256]) {
    for (int i = 0; i < 256; i++) {
        if (std::isalnum(static_cast<unsigned char>(i))) {
            lookUpTable[i] = static_cast<char>(~0);
        } else {
            lookUpTable[i] = 0;
        }
    }
}

std::vector<char*> tokenize(char* buffer, std::size_t bufferSize, char lookUpTable[256]) {
    std::vector<char*> tokens;

    if (bufferSize == 0) {
        return tokens;
    }

    char charPrev = lookUpTable[static_cast<unsigned char>(buffer[0])];
    buffer[0] &= charPrev;

    if (charPrev != 0) {
        tokens.push_back(&buffer[0]);
    }

    for (std::size_t i = 1; i < bufferSize; i++) {
        char charNext = lookUpTable[static_cast<unsigned char>(buffer[i])];
        buffer[i] &= charNext;

        if (charPrev == 0 && charNext != 0) {
            tokens.push_back(&buffer[i]);
        }

        charPrev = charNext;
    }

    return tokens;
}