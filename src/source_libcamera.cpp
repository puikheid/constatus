// (C) 2017-2021 by folkert van heusden, released under Apache License v2.0
#include "config.h"
#if HAVE_LIBCAMERA == 1
#include <errno.h>
#include <cstring>
#include <sys/mman.h>

#include "error.h"
#include "source_libcamera.h"
#include "log.h"
#include "utils.h"
#include "ptz_v4l.h"
#include "parameters.h"
#include "controls.h"

source_libcamera::source_libcamera(const std::string & id, const std::string & descr, const std::string & exec_failure, const std::string & dev, const int jpeg_quality, const double max_fps, const int w_requested, const int h_requested, resize *const r, const int resize_w, const int resize_h, const int loglevel, const double timeout, std::vector<filter *> *const filters, const failure_t & failure, const bool prefer_jpeg, const std::map<std::string, parameter *> & ctrls, controls *const c) : source(id, descr, exec_failure, max_fps, r, resize_w, resize_h, loglevel, timeout, filters, failure, c, jpeg_quality), dev(dev), w_requested(w_requested), h_requested(h_requested), prefer_jpeg(prefer_jpeg), ctrls(ctrls)
{
}

source_libcamera::~source_libcamera()
{
	stop();

	delete c;
}

void source_libcamera::request_completed(libcamera::Request *request)
{
	if (request->status() == libcamera::Request::RequestCancelled)
                return;

        const auto & buffers = request->buffers();

        for(auto bufferPair : buffers) {
		libcamera::FrameBuffer *const buffer = bufferPair.second;
                const libcamera::FrameMetadata & metadata = buffer->metadata();

		for(const libcamera::FrameBuffer::Plane & plane : buffer->planes()) {
			void *data = mappedBuffers[plane.fd.fd()].first;
			unsigned int length = plane.length;

			if (pixelformat == fourcc_code('M', 'J', 'P', 'G') || pixelformat == fourcc_code('J', 'P', 'E', 'G'))
				set_frame(E_JPEG, (const uint8_t *)data, length);
			else if (pixelformat == DRM_FORMAT_RGB888)
				set_frame(E_RGB, (const uint8_t *)data, length);
			else
				log(id, LL_ERR, "Unexpected pixelformat");

			break;
		}
        }

        request = camera->createRequest();
        if (!request) {
		log(id, LL_ERR, "Cannot create request for camera: stream will stall");
		return;
        }

        for(auto it = buffers.begin(); it != buffers.end(); ++it) {
		libcamera::Stream *stream = it->first;
		libcamera::FrameBuffer *buffer = it->second;

                request->addBuffer(stream, buffer);
        }

        camera->queueRequest(request);
}

void source_libcamera::operator()()
{
	log(id, LL_INFO, "source libcamera thread started");

	set_thread_name("src_libcamera");

	cm = new libcamera::CameraManager();
	
	if (int rc = cm->start(); rc != 0)
		error_exit(false, "libcamera: %s", strerror(-rc));

	camera = cm->get(dev);
	if (!camera)
		error_exit(false, "Camera \"%s\" not found", dev.c_str());

	if (camera->acquire())
		error_exit(false, "Cannot acquire \"%s\"", dev.c_str());

	log(id, LL_INFO, "Camera name: %s", camera->name().c_str());

	std::string controls_list;
        for(const auto & ctrl : camera->controls()) {
                const libcamera::ControlId *id = ctrl.first;

		controls_list += " " + ctrl.first->name();
	}

	log(id, LL_INFO, " Controls:%s", controls_list.c_str());

	const uint64_t interval = max_fps > 0.0 ? 1.0 / max_fps * 1000.0 * 1000.0 : 0;

	libcamera::StreamRoles roles{ libcamera::VideoRecording };
	std::unique_ptr<libcamera::CameraConfiguration> camera_config = camera->generateConfiguration(roles);

	size_t idx = 0;
	for(; idx<camera_config->size(); idx++) {
		uint32_t format = camera_config->at(idx).pixelFormat.fourcc();

		if (prefer_jpeg && (format == fourcc_code('M', 'J', 'P', 'G') || format == fourcc_code('J', 'P', 'E', 'G')))
			break;

		if (!prefer_jpeg && format == DRM_FORMAT_RGB888)
			break;
	}

	if (idx == camera_config->size())
		idx = 0;

	libcamera::StreamConfiguration & stream_config = camera_config->at(idx);
	stream_config.size.width = w_requested;
	stream_config.size.height = h_requested;

	stream_config.pixelFormat = libcamera::PixelFormat(DRM_FORMAT_RGB888);

	camera_config->validate();

	pixelformat = stream_config.pixelFormat.fourcc();
	log(id, LL_INFO, "Validated configuration is: %s (%c%c%c%c)", stream_config.toString().c_str(), pixelformat, pixelformat >> 8, pixelformat >> 16, pixelformat >> 24);

	std::unique_lock<std::mutex> lck(lock);
	width = stream_config.size.width;
	height = stream_config.size.height;
	lck.unlock();

	bool fail = false;

	if (camera->configure(camera_config.get())) {
		log(id, LL_ERR, "Cannot configure camera");

		fail = true;
	}
	else {
		stream = stream_config.stream();

		allocator = new libcamera::FrameBufferAllocator(camera);
		allocator->allocate(stream);
		log(id, LL_INFO, "Allocated %u buffers for stream", allocator->buffers(stream).size());

		const std::vector<std::unique_ptr<libcamera::FrameBuffer>> & buffers = allocator->buffers(stream);

		for(size_t i = 0; i < buffers.size(); i++) {
			libcamera::Request *request = camera->createRequest();
			if (!request) {
				fail = true;
				log(id, LL_ERR, "Can't create request for camera");
				break;
			}

			const std::unique_ptr<libcamera::FrameBuffer> & buffer = buffers[i];
			if (int ret = request->addBuffer(stream, buffer.get()); ret < 0) {
				fail = true;
				log(id, LL_ERR, "Can't set buffer for request: %s", strerror(-ret));
				break;
			}

			for(const libcamera::FrameBuffer::Plane & plane : buffer->planes()) {
				void *memory = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, plane.fd.fd(), 0);

				mappedBuffers[plane.fd.fd()] = std::make_pair(memory, plane.length);
			}

			for(const auto & ctrl : camera->controls()) {
				const libcamera::ControlId *id = ctrl.first;

				auto it = ctrls.find(str_tolower(ctrl.first->name()));

				if (it != ctrls.end()) {
					libcamera::ControlList & ctls = request->controls();

					if (id->type() == libcamera::ControlTypeFloat)
						ctls.set(id->id(), float(atof(it->second->get_value_string().c_str())));
					else if (id->type() == libcamera::ControlTypeString)
						ctls.set(id->id(), it->second->get_value_string());
				}
			}

			requests.push_back(request);
		}

		if (!fail) {
			camera->requestCompleted.connect(this, &source_libcamera::request_completed);

			camera->start();

			libcamera::EventDispatcher *const dispatcher = cm->eventDispatcher();

			for(auto request : requests)
				camera->queueRequest(request);

			for(;!local_stop_flag;) {
				libcamera::Timer timer;
				timer.start(100);

				while (timer.isRunning())
					dispatcher->processEvents();

				st->track_cpu_usage();
			}
		}
	}

	log(id, LL_INFO, "source libcamera thread terminating");

        camera->stop();
        allocator->free(stream);
        delete allocator;

        camera->release();
        cm->stop();

	register_thread_end("source libcamera");
}

void source_libcamera::pan_tilt(const double abs_pan, const double abs_tilt)
{
}

void source_libcamera::get_pan_tilt(double *const pan, double *const tilt) const
{
}
#endif
