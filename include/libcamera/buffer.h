/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * buffer.h - Buffer handling
 */
#ifndef __LIBCAMERA_BUFFER_H__
#define __LIBCAMERA_BUFFER_H__

#include <vector>

#include <libcamera/signal.h>

namespace libcamera {

class BufferPool;

class Plane final
{
public:
	Plane();
	~Plane();

	int dmabuf() const { return fd_; }
	int setDmabuf(int fd, unsigned int length);

	void *mem();
	unsigned int length() const { return length_; }

private:
	int mmap();
	int munmap();

	int fd_;
	unsigned int length_;
	void *mem_;
};

class Buffer final
{
public:
	Buffer();

	unsigned int index() const { return index_; }
	std::vector<Plane> &planes() { return planes_; }

	Signal<Buffer *> completed;

private:
	friend class BufferPool;

	unsigned int index_;

	std::vector<Plane> planes_;
};

class BufferPool final
{
public:
	~BufferPool();

	void createBuffers(unsigned int count);
	void destroyBuffers();

	unsigned int count() const { return buffers_.size(); }
	std::vector<Buffer> &buffers() { return buffers_; }

private:
	std::vector<Buffer> buffers_;
};

} /* namespace libcamera */

#endif /* __LIBCAMERA_BUFFER_H__ */