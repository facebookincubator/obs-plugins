/*
Copyright (C) 2019-present, Facebook, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include <obs-module.h>

#define OM_LOG(level, format, ...) \
	blog(level, "[OculusMrcSource]: " format, ##__VA_ARGS__)
#define OM_LOG_S(source, level, format, ...) \
	blog(level, "[OculusMrcSource '%s']: " format, \
			obs_source_get_name(source), ##__VA_ARGS__)

#define OM_BLOG(level, format, ...) \
	OM_LOG_S(this->m_src, level, format, ##__VA_ARGS__)

template<typename ... Args>
inline std::string string_format(const std::string& format, Args ... args)
{
	size_t size = snprintf(nullptr, 0, format.c_str(), args ...) + 1; // Extra space for '\0'
	std::unique_ptr<char[]> buf(new char[size]);
	snprintf(buf.get(), size, format.c_str(), args ...);
	return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}
