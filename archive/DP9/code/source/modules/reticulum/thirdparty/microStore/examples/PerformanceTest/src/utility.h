#pragma once

#include <microStore/Utility.h>

#undef abs
#undef round
#include <random>

/* -------------------------------------------------- */
/* RANDOM UTILITIES                                   */
/* -------------------------------------------------- */

std::mt19937 rng(microStore::time());

inline int rand_int(int max)
{
    std::uniform_int_distribution<int> d(0,max-1);
    return d(rng);
}

inline long rand_long(long max)
{
    std::uniform_int_distribution<long> d(0,max-1);
    return d(rng);
}
