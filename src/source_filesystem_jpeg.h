// (C) 2017-2020 by folkert van heusden, released under AGPL v3.0
#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "source.h"

class source_filesystem_jpeg : public source
{
private:
	const std::string path;

public:
	source_filesystem_jpeg(const std::string & id, const std::string & descr, const std::string & exec_failure, const std::string & path, const double fps, resize *const r, const int resize_w, const int resize_h, const int loglevel, std::vector<filter *> *const filters, const failure_t & failure, controls *const c, const int jpeg_quality);
	~source_filesystem_jpeg();

	void operator()() override;
};
