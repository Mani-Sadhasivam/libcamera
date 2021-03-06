/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * formats.h - Libcamera image formats
 */

#ifndef __LIBCAMERA_FORMATS_H__
#define __LIBCAMERA_FORMATS_H__

#include <map>
#include <vector>

#include <libcamera/geometry.h>

namespace libcamera {

class ImageFormats
{
public:
	int addFormat(unsigned int format, const std::vector<SizeRange> &sizes);

	bool isEmpty() const;
	std::vector<unsigned int> formats() const;
	const std::vector<SizeRange> &sizes(unsigned int format) const;
	const std::map<unsigned int, std::vector<SizeRange>> &data() const;

private:
	std::map<unsigned int, std::vector<SizeRange>> data_;
};

} /* namespace libcamera */

#endif /* __LIBCAMERA_FORMATS_H__ */
