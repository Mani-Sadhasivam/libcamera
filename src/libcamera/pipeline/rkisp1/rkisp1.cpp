/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * rkisp1.cpp - Pipeline handler for Rockchip ISP1
 */

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <queue>

#include <linux/media-bus-format.h>

#include <ipa/rkisp1.h>
#include <libcamera/buffer.h>
#include <libcamera/camera.h>
#include <libcamera/control_ids.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include "camera_sensor.h"
#include "device_enumerator.h"
#include "ipa_manager.h"
#include "log.h"
#include "media_device.h"
#include "pipeline_handler.h"
#include "timeline.h"
#include "utils.h"
#include "v4l2_subdevice.h"
#include "v4l2_videodevice.h"

#define RKISP1_PARAM_BASE 0x100
#define RKISP1_STAT_BASE 0x200

namespace libcamera {

LOG_DEFINE_CATEGORY(RkISP1)

class PipelineHandlerRkISP1;
class RkISP1ActionQueueBuffers;

enum RkISP1ActionType {
	SetSensor,
	SOE,
	QueueBuffers,
};

struct RkISP1FrameInfo {
	unsigned int frame;
	Request *request;

	Buffer *paramBuffer;
	Buffer *statBuffer;
	Buffer *videoBuffer;

	bool paramFilled;
	bool paramDequeued;
	bool metadataProcessed;
};

class RkISP1Frames
{
public:
	RkISP1Frames(PipelineHandler *pipe);

	RkISP1FrameInfo *create(unsigned int frame, Request *request, Stream *stream);
	int destroy(unsigned int frame);

	RkISP1FrameInfo *find(unsigned int frame);
	RkISP1FrameInfo *find(Buffer *buffer);
	RkISP1FrameInfo *find(Request *request);

private:
	PipelineHandlerRkISP1 *pipe_;
	std::map<unsigned int, RkISP1FrameInfo *> frameInfo_;
};

class RkISP1Timeline : public Timeline
{
public:
	RkISP1Timeline()
		: Timeline()
	{
		setDelay(SetSensor, -1, 5);
		setDelay(SOE, 0, -1);
		setDelay(QueueBuffers, -1, 10);
	}

	void bufferReady(Buffer *buffer)
	{
		/*
		 * Calculate SOE by taking the end of DMA set by the kernel and applying
		 * the time offsets provideprovided by the IPA to find the best estimate
		 * of SOE.
		 */

		ASSERT(frameOffset(SOE) == 0);

		utils::time_point soe = std::chrono::time_point<utils::clock>()
			+ std::chrono::nanoseconds(buffer->timestamp())
			+ timeOffset(SOE);

		notifyStartOfExposure(buffer->sequence(), soe);
	}

	void setDelay(unsigned int type, int frame, int msdelay)
	{
		utils::duration delay = std::chrono::milliseconds(msdelay);
		setRawDelay(type, frame, delay);
	}
};

class RkISP1CameraData : public CameraData
{
public:
	RkISP1CameraData(PipelineHandler *pipe)
		: CameraData(pipe), sensor_(nullptr), frame_(0),
		  frameInfo_(pipe)
	{
	}

	~RkISP1CameraData()
	{
		delete sensor_;
	}

	int loadIPA();

	Stream stream_;
	CameraSensor *sensor_;
	unsigned int frame_;
	std::vector<IPABuffer> ipaBuffers_;
	RkISP1Frames frameInfo_;
	RkISP1Timeline timeline_;

private:
	void queueFrameAction(unsigned int frame,
			      const IPAOperationData &action);

	void metadataReady(unsigned int frame, const ControlList &metadata);
};

class RkISP1CameraConfiguration : public CameraConfiguration
{
public:
	RkISP1CameraConfiguration(Camera *camera, RkISP1CameraData *data);

	Status validate() override;

	const V4L2SubdeviceFormat &sensorFormat() { return sensorFormat_; }

private:
	static constexpr unsigned int RKISP1_BUFFER_COUNT = 4;

	/*
	 * The RkISP1CameraData instance is guaranteed to be valid as long as the
	 * corresponding Camera instance is valid. In order to borrow a
	 * reference to the camera data, store a new reference to the camera.
	 */
	std::shared_ptr<Camera> camera_;
	const RkISP1CameraData *data_;

	V4L2SubdeviceFormat sensorFormat_;
};

class PipelineHandlerRkISP1 : public PipelineHandler
{
public:
	PipelineHandlerRkISP1(CameraManager *manager);
	~PipelineHandlerRkISP1();

	CameraConfiguration *generateConfiguration(Camera *camera,
		const StreamRoles &roles) override;
	int configure(Camera *camera, CameraConfiguration *config) override;

	int allocateBuffers(Camera *camera,
		const std::set<Stream *> &streams) override;
	int freeBuffers(Camera *camera,
		const std::set<Stream *> &streams) override;

	int start(Camera *camera) override;
	void stop(Camera *camera) override;

	int queueRequest(Camera *camera, Request *request) override;

	bool match(DeviceEnumerator *enumerator) override;

private:
	RkISP1CameraData *cameraData(const Camera *camera)
	{
		return static_cast<RkISP1CameraData *>(
			PipelineHandler::cameraData(camera));
	}

	friend RkISP1ActionQueueBuffers;
	friend RkISP1CameraData;
	friend RkISP1Frames;

	int initLinks();
	int createCamera(MediaEntity *sensor);
	void tryCompleteRequest(Request *request);
	void bufferReady(Buffer *buffer);
	void paramReady(Buffer *buffer);
	void statReady(Buffer *buffer);

	MediaDevice *media_;
	V4L2Subdevice *dphy_;
	V4L2Subdevice *isp_;
	V4L2VideoDevice *video_;
	V4L2VideoDevice *param_;
	V4L2VideoDevice *stat_;

	BufferPool paramPool_;
	BufferPool statPool_;

	std::queue<Buffer *> paramBuffers_;
	std::queue<Buffer *> statBuffers_;

	Camera *activeCamera_;
};

RkISP1Frames::RkISP1Frames(PipelineHandler *pipe)
	: pipe_(dynamic_cast<PipelineHandlerRkISP1 *>(pipe))
{
}

RkISP1FrameInfo *RkISP1Frames::create(unsigned int frame, Request *request, Stream *stream)
{
	if (pipe_->paramBuffers_.empty()) {
		LOG(RkISP1, Error) << "Parameters buffer underrun";
		return nullptr;
	}
	Buffer *paramBuffer = pipe_->paramBuffers_.front();

	if (pipe_->statBuffers_.empty()) {
		LOG(RkISP1, Error) << "Statisitc buffer underrun";
		return nullptr;
	}
	Buffer *statBuffer = pipe_->statBuffers_.front();

	Buffer *videoBuffer = request->findBuffer(stream);
	if (!videoBuffer) {
		LOG(RkISP1, Error)
			<< "Attempt to queue request with invalid stream";
		return nullptr;
	}

	pipe_->paramBuffers_.pop();
	pipe_->statBuffers_.pop();

	RkISP1FrameInfo *info = new RkISP1FrameInfo;

	info->frame = frame;
	info->request = request;
	info->paramBuffer = paramBuffer;
	info->videoBuffer = videoBuffer;
	info->statBuffer = statBuffer;
	info->paramFilled = false;
	info->paramDequeued = false;
	info->metadataProcessed = false;

	frameInfo_[frame] = info;

	return info;
}

int RkISP1Frames::destroy(unsigned int frame)
{
	RkISP1FrameInfo *info = find(frame);
	if (!info)
		return -ENOENT;

	pipe_->paramBuffers_.push(info->paramBuffer);
	pipe_->statBuffers_.push(info->statBuffer);

	frameInfo_.erase(info->frame);

	delete info;

	return 0;
}

RkISP1FrameInfo *RkISP1Frames::find(unsigned int frame)
{
	auto itInfo = frameInfo_.find(frame);

	if (itInfo != frameInfo_.end())
		return itInfo->second;

	LOG(RkISP1, Error) << "Can't locate info from frame";
	return nullptr;
}

RkISP1FrameInfo *RkISP1Frames::find(Buffer *buffer)
{
	for (auto &itInfo : frameInfo_) {
		RkISP1FrameInfo *info = itInfo.second;

		if (info->paramBuffer == buffer ||
		    info->statBuffer == buffer ||
		    info->videoBuffer == buffer)
			return info;
	}

	LOG(RkISP1, Error) << "Can't locate info from buffer";
	return nullptr;
}

RkISP1FrameInfo *RkISP1Frames::find(Request *request)
{
	for (auto &itInfo : frameInfo_) {
		RkISP1FrameInfo *info = itInfo.second;

		if (info->request == request)
			return info;
	}

	LOG(RkISP1, Error) << "Can't locate info from request";
	return nullptr;
}

class RkISP1ActionSetSensor : public FrameAction
{
public:
	RkISP1ActionSetSensor(unsigned int frame, CameraSensor *sensor, const ControlList &controls)
		: FrameAction(frame, SetSensor), sensor_(sensor), controls_(controls) {}

protected:
	void run() override
	{
		sensor_->setControls(&controls_);
	}

private:
	CameraSensor *sensor_;
	ControlList controls_;
};

class RkISP1ActionQueueBuffers : public FrameAction
{
public:
	RkISP1ActionQueueBuffers(unsigned int frame, RkISP1CameraData *data,
				 PipelineHandlerRkISP1 *pipe)
		: FrameAction(frame, QueueBuffers), data_(data), pipe_(pipe)
	{
	}

protected:
	void run() override
	{
		RkISP1FrameInfo *info = data_->frameInfo_.find(frame());
		if (!info)
			LOG(RkISP1, Fatal) << "Frame not known";

		if (info->paramFilled)
			pipe_->param_->queueBuffer(info->paramBuffer);
		else
			LOG(RkISP1, Error)
				<< "Parameters not ready on time for frame "
				<< frame() << ", ignore parameters.";

		pipe_->stat_->queueBuffer(info->statBuffer);
		pipe_->video_->queueBuffer(info->videoBuffer);
	}

private:
	RkISP1CameraData *data_;
	PipelineHandlerRkISP1 *pipe_;
};

int RkISP1CameraData::loadIPA()
{
	ipa_ = IPAManager::instance()->createIPA(pipe_, 1, 1);
	if (!ipa_)
		return -ENOENT;

	ipa_->queueFrameAction.connect(this,
				       &RkISP1CameraData::queueFrameAction);

	return 0;
}

void RkISP1CameraData::queueFrameAction(unsigned int frame,
					const IPAOperationData &action)
{
	switch (action.operation) {
	case RKISP1_IPA_ACTION_V4L2_SET: {
		const ControlList &controls = action.controls[0];
		timeline_.scheduleAction(utils::make_unique<RkISP1ActionSetSensor>(frame,
										   sensor_,
										   controls));
		break;
	}
	case RKISP1_IPA_ACTION_PARAM_FILLED: {
		RkISP1FrameInfo *info = frameInfo_.find(frame);
		if (info)
			info->paramFilled = true;
		break;
	}
	case RKISP1_IPA_ACTION_METADATA:
		metadataReady(frame, action.controls[0]);
		break;
	default:
		LOG(RkISP1, Error) << "Unkown action " << action.operation;
		break;
	}
}

void RkISP1CameraData::metadataReady(unsigned int frame, const ControlList &metadata)
{
	PipelineHandlerRkISP1 *pipe =
		static_cast<PipelineHandlerRkISP1 *>(pipe_);

	RkISP1FrameInfo *info = frameInfo_.find(frame);
	if (!info)
		return;

	info->request->metadata() = metadata;
	info->metadataProcessed = true;

	pipe->tryCompleteRequest(info->request);
}

RkISP1CameraConfiguration::RkISP1CameraConfiguration(Camera *camera,
						     RkISP1CameraData *data)
	: CameraConfiguration()
{
	camera_ = camera->shared_from_this();
	data_ = data;
}

CameraConfiguration::Status RkISP1CameraConfiguration::validate()
{
	static const std::array<unsigned int, 8> formats{
		V4L2_PIX_FMT_YUYV,
		V4L2_PIX_FMT_YVYU,
		V4L2_PIX_FMT_VYUY,
		V4L2_PIX_FMT_NV16,
		V4L2_PIX_FMT_NV61,
		V4L2_PIX_FMT_NV21,
		V4L2_PIX_FMT_NV12,
		V4L2_PIX_FMT_GREY,
	};

	const CameraSensor *sensor = data_->sensor_;
	Status status = Valid;

	if (config_.empty())
		return Invalid;

	/* Cap the number of entries to the available streams. */
	if (config_.size() > 1) {
		config_.resize(1);
		status = Adjusted;
	}

	StreamConfiguration &cfg = config_[0];

	/* Adjust the pixel format. */
	if (std::find(formats.begin(), formats.end(), cfg.pixelFormat) ==
	    formats.end()) {
		LOG(RkISP1, Debug) << "Adjusting format to NV12";
		cfg.pixelFormat = V4L2_PIX_FMT_NV12;
		status = Adjusted;
	}

	/* Select the sensor format. */
	sensorFormat_ = sensor->getFormat({ MEDIA_BUS_FMT_SBGGR12_1X12,
					    MEDIA_BUS_FMT_SGBRG12_1X12,
					    MEDIA_BUS_FMT_SGRBG12_1X12,
					    MEDIA_BUS_FMT_SRGGB12_1X12,
					    MEDIA_BUS_FMT_SBGGR10_1X10,
					    MEDIA_BUS_FMT_SGBRG10_1X10,
					    MEDIA_BUS_FMT_SGRBG10_1X10,
					    MEDIA_BUS_FMT_SRGGB10_1X10,
					    MEDIA_BUS_FMT_SBGGR8_1X8,
					    MEDIA_BUS_FMT_SGBRG8_1X8,
					    MEDIA_BUS_FMT_SGRBG8_1X8,
					    MEDIA_BUS_FMT_SRGGB8_1X8 },
					  cfg.size);
	if (!sensorFormat_.size.width || !sensorFormat_.size.height)
		sensorFormat_.size = sensor->resolution();

	/*
	 * Provide a suitable default that matches the sensor aspect
	 * ratio and clamp the size to the hardware bounds.
	 *
	 * \todo: Check the hardware alignment constraints.
	 */
	const Size size = cfg.size;

	if (!cfg.size.width || !cfg.size.height) {
		cfg.size.width = 1280;
		cfg.size.height = 1280 * sensorFormat_.size.height
				/ sensorFormat_.size.width;
	}

	cfg.size.width = std::max(32U, std::min(4416U, cfg.size.width));
	cfg.size.height = std::max(16U, std::min(3312U, cfg.size.height));

	if (cfg.size != size) {
		LOG(RkISP1, Debug)
			<< "Adjusting size from " << size.toString()
			<< " to " << cfg.size.toString();
		status = Adjusted;
	}

	cfg.bufferCount = RKISP1_BUFFER_COUNT;

	return status;
}

PipelineHandlerRkISP1::PipelineHandlerRkISP1(CameraManager *manager)
	: PipelineHandler(manager), dphy_(nullptr), isp_(nullptr),
	  video_(nullptr), param_(nullptr), stat_(nullptr)
{
}

PipelineHandlerRkISP1::~PipelineHandlerRkISP1()
{
	delete param_;
	delete stat_;
	delete video_;
	delete isp_;
	delete dphy_;
}

/* -----------------------------------------------------------------------------
 * Pipeline Operations
 */

CameraConfiguration *PipelineHandlerRkISP1::generateConfiguration(Camera *camera,
	const StreamRoles &roles)
{
	RkISP1CameraData *data = cameraData(camera);
	CameraConfiguration *config = new RkISP1CameraConfiguration(camera, data);

	if (roles.empty())
		return config;

	StreamConfiguration cfg{};
	cfg.pixelFormat = V4L2_PIX_FMT_NV12;
	cfg.size = data->sensor_->resolution();

	config->addConfiguration(cfg);

	config->validate();

	return config;
}

int PipelineHandlerRkISP1::configure(Camera *camera, CameraConfiguration *c)
{
	RkISP1CameraConfiguration *config =
		static_cast<RkISP1CameraConfiguration *>(c);
	RkISP1CameraData *data = cameraData(camera);
	StreamConfiguration &cfg = config->at(0);
	CameraSensor *sensor = data->sensor_;
	int ret;

	/*
	 * Configure the sensor links: enable the link corresponding to this
	 * camera and disable all the other sensor links.
	 */
	const MediaPad *pad = dphy_->entity()->getPadByIndex(0);

	for (MediaLink *link : pad->links()) {
		bool enable = link->source()->entity() == sensor->entity();

		if (!!(link->flags() & MEDIA_LNK_FL_ENABLED) == enable)
			continue;

		LOG(RkISP1, Debug)
			<< (enable ? "Enabling" : "Disabling")
			<< " link from sensor '"
			<< link->source()->entity()->name()
			<< "' to CSI-2 receiver";

		ret = link->setEnabled(enable);
		if (ret < 0)
			return ret;
	}

	/*
	 * Configure the format on the sensor output and propagate it through
	 * the pipeline.
	 */
	V4L2SubdeviceFormat format = config->sensorFormat();
	LOG(RkISP1, Debug) << "Configuring sensor with " << format.toString();

	ret = sensor->setFormat(&format);
	if (ret < 0)
		return ret;

	LOG(RkISP1, Debug) << "Sensor configured with " << format.toString();

	ret = dphy_->setFormat(0, &format);
	if (ret < 0)
		return ret;

	LOG(RkISP1, Debug) << "Configuring ISP input pad with " << format.toString();

	ret = dphy_->getFormat(1, &format);
	if (ret < 0)
		return ret;

	ret = isp_->setFormat(0, &format);
	if (ret < 0)
		return ret;

	LOG(RkISP1, Debug) << "ISP input pad configured with " << format.toString();

	/* YUYV8_2X8 is required on the ISP source path pad for YUV output. */
	format.mbus_code = MEDIA_BUS_FMT_YUYV8_2X8;
	LOG(RkISP1, Debug) << "Configuring ISP output pad with " << format.toString();

	ret = isp_->setFormat(2, &format);
	if (ret < 0)
		return ret;

	LOG(RkISP1, Debug) << "ISP output pad configured with " << format.toString();

	V4L2DeviceFormat outputFormat = {};
	outputFormat.fourcc = cfg.pixelFormat;
	outputFormat.size = cfg.size;
	outputFormat.planesCount = 2;

	ret = video_->setFormat(&outputFormat);
	if (ret)
		return ret;

	if (outputFormat.size != cfg.size ||
	    outputFormat.fourcc != cfg.pixelFormat) {
		LOG(RkISP1, Error)
			<< "Unable to configure capture in " << cfg.toString();
		return -EINVAL;
	}

	V4L2DeviceFormat paramFormat = {};
	paramFormat.fourcc = V4L2_META_FMT_RK_ISP1_PARAMS;
	ret = param_->setFormat(&paramFormat);
	if (ret)
		return ret;

	V4L2DeviceFormat statFormat = {};
	statFormat.fourcc = V4L2_META_FMT_RK_ISP1_STAT_3A;
	ret = stat_->setFormat(&statFormat);
	if (ret)
		return ret;

	cfg.setStream(&data->stream_);

	return 0;
}

int PipelineHandlerRkISP1::allocateBuffers(Camera *camera,
					   const std::set<Stream *> &streams)
{
	RkISP1CameraData *data = cameraData(camera);
	Stream *stream = *streams.begin();
	int ret;

	if (stream->memoryType() == InternalMemory)
		ret = video_->exportBuffers(&stream->bufferPool());
	else
		ret = video_->importBuffers(&stream->bufferPool());

	if (ret)
		return ret;

	paramPool_.createBuffers(stream->configuration().bufferCount + 1);
	ret = param_->exportBuffers(&paramPool_);
	if (ret) {
		video_->releaseBuffers();
		return ret;
	}

	statPool_.createBuffers(stream->configuration().bufferCount + 1);
	ret = stat_->exportBuffers(&statPool_);
	if (ret) {
		param_->releaseBuffers();
		video_->releaseBuffers();
		return ret;
	}

	for (unsigned int i = 0; i < stream->configuration().bufferCount + 1; i++) {
		data->ipaBuffers_.push_back({ .id = RKISP1_PARAM_BASE | i,
					      .memory = paramPool_.buffers()[i] });
		paramBuffers_.push(new Buffer(i));
	}

	for (unsigned int i = 0; i < stream->configuration().bufferCount + 1; i++) {
		data->ipaBuffers_.push_back({ .id = RKISP1_STAT_BASE | i,
					      .memory = statPool_.buffers()[i] });
		statBuffers_.push(new Buffer(i));
	}

	data->ipa_->mapBuffers(data->ipaBuffers_);

	return ret;
}

int PipelineHandlerRkISP1::freeBuffers(Camera *camera,
				       const std::set<Stream *> &streams)
{
	RkISP1CameraData *data = cameraData(camera);

	while (!statBuffers_.empty()) {
		delete statBuffers_.front();
		statBuffers_.pop();
	}

	while (!paramBuffers_.empty()) {
		delete paramBuffers_.front();
		paramBuffers_.pop();
	}

	std::vector<unsigned int> ids;
	for (IPABuffer &ipabuf : data->ipaBuffers_)
		ids.push_back(ipabuf.id);

	data->ipa_->unmapBuffers(ids);
	data->ipaBuffers_.clear();

	if (param_->releaseBuffers())
		LOG(RkISP1, Error) << "Failed to release parameters buffers";

	if (stat_->releaseBuffers())
		LOG(RkISP1, Error) << "Failed to release stat buffers";

	if (video_->releaseBuffers())
		LOG(RkISP1, Error) << "Failed to release video buffers";

	return 0;
}

int PipelineHandlerRkISP1::start(Camera *camera)
{
	RkISP1CameraData *data = cameraData(camera);
	int ret;

	data->frame_ = 0;

	ret = param_->streamOn();
	if (ret) {
		LOG(RkISP1, Error)
			<< "Failed to start parameters " << camera->name();
		return ret;
	}

	ret = stat_->streamOn();
	if (ret) {
		param_->streamOff();
		LOG(RkISP1, Error)
			<< "Failed to start statistics " << camera->name();
		return ret;
	}

	ret = video_->streamOn();
	if (ret) {
		param_->streamOff();
		stat_->streamOff();

		LOG(RkISP1, Error)
			<< "Failed to start camera " << camera->name();
	}

	activeCamera_ = camera;

	/* Inform IPA of stream configuration and sensor controls. */
	std::map<unsigned int, IPAStream> streamConfig;
	streamConfig[0] = {
		.pixelFormat = data->stream_.configuration().pixelFormat,
		.size = data->stream_.configuration().size,
	};

	std::map<unsigned int, ControlInfoMap> entityControls;
	entityControls.emplace(0, data->sensor_->controls());

	data->ipa_->configure(streamConfig, entityControls);

	return ret;
}

void PipelineHandlerRkISP1::stop(Camera *camera)
{
	RkISP1CameraData *data = cameraData(camera);
	int ret;

	ret = video_->streamOff();
	if (ret)
		LOG(RkISP1, Warning)
			<< "Failed to stop camera " << camera->name();

	ret = stat_->streamOff();
	if (ret)
		LOG(RkISP1, Warning)
			<< "Failed to stop statistics " << camera->name();

	ret = param_->streamOff();
	if (ret)
		LOG(RkISP1, Warning)
			<< "Failed to stop parameters " << camera->name();

	data->timeline_.reset();

	activeCamera_ = nullptr;
}

int PipelineHandlerRkISP1::queueRequest(Camera *camera, Request *request)
{
	RkISP1CameraData *data = cameraData(camera);
	Stream *stream = &data->stream_;

	PipelineHandler::queueRequest(camera, request);

	RkISP1FrameInfo *info = data->frameInfo_.create(data->frame_, request,
							stream);
	if (!info)
		return -ENOENT;

	IPAOperationData op;
	op.operation = RKISP1_IPA_EVENT_QUEUE_REQUEST;
	op.data = { data->frame_, RKISP1_PARAM_BASE | info->paramBuffer->index() };
	op.controls = { request->controls() };
	data->ipa_->processEvent(op);

	data->timeline_.scheduleAction(utils::make_unique<RkISP1ActionQueueBuffers>(data->frame_,
										    data,
										    this));

	data->frame_++;

	return 0;
}

/* -----------------------------------------------------------------------------
 * Match and Setup
 */

int PipelineHandlerRkISP1::initLinks()
{
	MediaLink *link;
	int ret;

	ret = media_->disableLinks();
	if (ret < 0)
		return ret;

	link = media_->link("rockchip-sy-mipi-dphy", 1, "rkisp1-isp-subdev", 0);
	if (!link)
		return -ENODEV;

	ret = link->setEnabled(true);
	if (ret < 0)
		return ret;

	link = media_->link("rkisp1-isp-subdev", 2, "rkisp1_mainpath", 0);
	if (!link)
		return -ENODEV;

	ret = link->setEnabled(true);
	if (ret < 0)
		return ret;

	return 0;
}

int PipelineHandlerRkISP1::createCamera(MediaEntity *sensor)
{
	int ret;

	std::unique_ptr<RkISP1CameraData> data =
		utils::make_unique<RkISP1CameraData>(this);

	ControlInfoMap::Map ctrls;
	ctrls.emplace(std::piecewise_construct,
		      std::forward_as_tuple(&controls::AeEnable),
		      std::forward_as_tuple(false, true));

	data->controlInfo_ = std::move(ctrls);

	data->sensor_ = new CameraSensor(sensor);
	ret = data->sensor_->init();
	if (ret)
		return ret;

	ret = data->loadIPA();
	if (ret)
		return ret;

	std::set<Stream *> streams{ &data->stream_ };
	std::shared_ptr<Camera> camera =
		Camera::create(this, sensor->name(), streams);
	registerCamera(std::move(camera), std::move(data));

	return 0;
}

bool PipelineHandlerRkISP1::match(DeviceEnumerator *enumerator)
{
	const MediaPad *pad;

	DeviceMatch dm("rkisp1");
	dm.add("rkisp1-isp-subdev");
	dm.add("rkisp1_selfpath");
	dm.add("rkisp1_mainpath");
	dm.add("rkisp1-statistics");
	dm.add("rkisp1-input-params");
	dm.add("rockchip-sy-mipi-dphy");

	media_ = acquireMediaDevice(enumerator, dm);
	if (!media_)
		return false;

	/* Create the V4L2 subdevices we will need. */
	dphy_ = V4L2Subdevice::fromEntityName(media_, "rockchip-sy-mipi-dphy");
	if (dphy_->open() < 0)
		return false;

	isp_ = V4L2Subdevice::fromEntityName(media_, "rkisp1-isp-subdev");
	if (isp_->open() < 0)
		return false;

	/* Locate and open the capture video node. */
	video_ = V4L2VideoDevice::fromEntityName(media_, "rkisp1_mainpath");
	if (video_->open() < 0)
		return false;

	stat_ = V4L2VideoDevice::fromEntityName(media_, "rkisp1-statistics");
	if (stat_->open() < 0)
		return false;

	param_ = V4L2VideoDevice::fromEntityName(media_, "rkisp1-input-params");
	if (param_->open() < 0)
		return false;

	video_->bufferReady.connect(this, &PipelineHandlerRkISP1::bufferReady);
	stat_->bufferReady.connect(this, &PipelineHandlerRkISP1::statReady);
	param_->bufferReady.connect(this, &PipelineHandlerRkISP1::paramReady);

	/* Configure default links. */
	if (initLinks() < 0) {
		LOG(RkISP1, Error) << "Failed to setup links";
		return false;
	}

	/*
	 * Enumerate all sensors connected to the CSI-2 receiver and create one
	 * camera instance for each of them.
	 */
	pad = dphy_->entity()->getPadByIndex(0);
	if (!pad)
		return false;

	for (MediaLink *link : pad->links())
		createCamera(link->source()->entity());

	return true;
}

/* -----------------------------------------------------------------------------
 * Buffer Handling
 */

void PipelineHandlerRkISP1::tryCompleteRequest(Request *request)
{
	RkISP1CameraData *data = cameraData(activeCamera_);
	RkISP1FrameInfo *info = data->frameInfo_.find(request);
	if (!info)
		return;

	if (request->hasPendingBuffers())
		return;

	if (!info->metadataProcessed)
		return;

	if (!info->paramDequeued)
		return;

	completeRequest(activeCamera_, request);

	data->frameInfo_.destroy(info->frame);
}

void PipelineHandlerRkISP1::bufferReady(Buffer *buffer)
{
	ASSERT(activeCamera_);
	RkISP1CameraData *data = cameraData(activeCamera_);
	Request *request = buffer->request();

	data->timeline_.bufferReady(buffer);

	if (data->frame_ <= buffer->sequence())
		data->frame_ = buffer->sequence() + 1;

	completeBuffer(activeCamera_, request, buffer);
	tryCompleteRequest(request);
}

void PipelineHandlerRkISP1::paramReady(Buffer *buffer)
{
	ASSERT(activeCamera_);
	RkISP1CameraData *data = cameraData(activeCamera_);

	RkISP1FrameInfo *info = data->frameInfo_.find(buffer);

	info->paramDequeued = true;
	tryCompleteRequest(info->request);
}

void PipelineHandlerRkISP1::statReady(Buffer *buffer)
{
	ASSERT(activeCamera_);
	RkISP1CameraData *data = cameraData(activeCamera_);

	RkISP1FrameInfo *info = data->frameInfo_.find(buffer);
	if (!info)
		return;

	unsigned int frame = info->frame;
	unsigned int statid = RKISP1_STAT_BASE | info->statBuffer->index();

	IPAOperationData op;
	op.operation = RKISP1_IPA_EVENT_SIGNAL_STAT_BUFFER;
	op.data = { frame, statid };
	data->ipa_->processEvent(op);
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerRkISP1);

} /* namespace libcamera */
