/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/config.h"

#include <cstddef>
#include <new>

namespace mongo {
namespace stdx {

// This is a c++17 library feature which is implemented until GCC 12.
// So it does not work if you only compile with flags "-std=c++17"
// Refer to https://en.cppreference.com/w/cpp/compiler_support/17
#if __cplusplus < 201703L || \
    !(defined(__cpp_lib_hardware_interference_size) && !defined(_LIBCPP_VERSION))

#if defined(MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT)
static_assert(MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT >= sizeof(uint64_t), "Bad extended alignment");
constexpr std::size_t hardware_destructive_interference_size = MONGO_CONFIG_MAX_EXTENDED_ALIGNMENT;
#else
constexpr std::size_t hardware_destructive_interference_size = alignof(std::max_align_t);
#endif

constexpr auto hardware_constructive_interference_size = hardware_destructive_interference_size;

#else

using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;

#endif

}  // namespace stdx
}  // namespace mongo
