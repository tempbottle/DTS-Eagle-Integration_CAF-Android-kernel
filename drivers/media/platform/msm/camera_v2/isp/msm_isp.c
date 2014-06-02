/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <linux/videodev2.h>
#include <linux/of_device.h>
#include <linux/qcom_iommu.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>

#include "msm_isp.h"
#include "msm_isp_util.h"
#include "msm_isp_axi_util.h"
#include "msm_isp_stats_util.h"
#include "msm_sd.h"
#include "msm_isp46.h"
#include "msm_isp44.h"
#include "msm_isp40.h"
#include "msm_isp32.h"

static struct msm_sd_req_vb2_q vfe_vb2_ops;

static const struct of_device_id msm_vfe_dt_match[] = {
	{
		.compatible = "qcom,vfe46",
		.data = &vfe46_hw_info,
	},
	{
		.compatible = "qcom,vfe44",
		.data = &vfe44_hw_info,
	},
	{
		.compatible = "qcom,vfe40",
		.data = &vfe40_hw_info,
	},
	{
		.compatible = "qcom,vfe32",
		.data = &vfe32_hw_info,
	},
	{}
};

MODULE_DEVICE_TABLE(of, msm_vfe_dt_match);

static const struct platform_device_id msm_vfe_dev_id[] = {
	{"msm_vfe32", (kernel_ulong_t) &vfe32_hw_info},
	{}
};

static struct msm_isp_buf_mgr vfe_buf_mgr;

#ifdef CONFIG_COMPAT
struct msm_isp_event_data32 {
	struct compat_timeval timestamp;
	struct compat_timeval mono_timestamp;
	enum msm_vfe_input_src input_intf;
	uint32_t frame_id;
	union {
		struct msm_isp_stats_event stats;
		struct msm_isp_buf_event buf_done;
	} u;
};
static long msm_isp_dqevent(struct file *file, struct v4l2_fh *vfh, void *arg)
{
	long rc;
	if (is_compat_task()) {
		struct msm_isp_event_data32 *event_data32;
		struct msm_isp_event_data  *event_data;
		struct v4l2_event isp_event;
		struct v4l2_event *isp_event_user;

		memset(&isp_event, 0, sizeof(isp_event));
		rc = v4l2_event_dequeue(vfh, &isp_event,
				file->f_flags & O_NONBLOCK);
		if (rc)
			return rc;
		event_data = (struct msm_isp_event_data *)
				isp_event.u.data;
		isp_event_user = (struct v4l2_event *)arg;
		memcpy(isp_event_user, &isp_event,
				sizeof(*isp_event_user));
		event_data32 = (struct msm_isp_event_data32 *)
			isp_event_user->u.data;
		memset(event_data32, 0,
				sizeof(struct msm_isp_event_data32));
		event_data32->timestamp.tv_sec =
				event_data->timestamp.tv_sec;
		event_data32->timestamp.tv_usec =
				event_data->timestamp.tv_usec;
		event_data32->mono_timestamp.tv_sec =
				event_data->mono_timestamp.tv_sec;
		event_data32->mono_timestamp.tv_usec =
				event_data->mono_timestamp.tv_usec;
		event_data32->input_intf = event_data->input_intf;
		event_data32->frame_id = event_data->frame_id;
		memcpy(&(event_data32->u), &(event_data->u),
					sizeof(event_data32->u));
	} else {
		rc = v4l2_event_dequeue(vfh, arg,
				file->f_flags & O_NONBLOCK);
	}
	return rc;
}
#else
static long msm_isp_dqevent(struct file *file, struct v4l2_fh *vfh, void *arg)
{
	return v4l2_event_dequeue(vfh, arg,
			file->f_flags & O_NONBLOCK);
}
#endif

static long msm_isp_subdev_do_ioctl(
	struct file *file, unsigned int cmd, void *arg)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_subdev *sd = vdev_to_v4l2_subdev(vdev);
	struct v4l2_fh *vfh = file->private_data;

	switch (cmd) {
	case VIDIOC_DQEVENT: {
		if (!(sd->flags & V4L2_SUBDEV_FL_HAS_EVENTS))
			return -ENOIOCTLCMD;
		return msm_isp_dqevent(file, vfh, arg);
	}
	break;
	case VIDIOC_SUBSCRIBE_EVENT:
		return v4l2_subdev_call(sd, core, subscribe_event, vfh, arg);

	case VIDIOC_UNSUBSCRIBE_EVENT:
		return v4l2_subdev_call(sd, core, unsubscribe_event, vfh, arg);

	default:
		return v4l2_subdev_call(sd, core, ioctl, cmd, arg);
	}
}

static long msm_isp_subdev_fops_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_isp_subdev_do_ioctl);
}

static struct v4l2_file_operations msm_isp_v4l2_subdev_fops = {
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = msm_isp_subdev_fops_ioctl,
#endif
	.unlocked_ioctl = msm_isp_subdev_fops_ioctl
};

static int vfe_probe(struct platform_device *pdev)
{
	struct vfe_device *vfe_dev;
	/*struct msm_cam_subdev_info sd_info;*/
	const struct of_device_id *match_dev;
	int rc = 0;

	struct msm_iova_partition vfe_partition = {
		.start = SZ_128K,
		.size = SZ_2G - SZ_128K,
	};
	struct msm_iova_layout vfe_layout = {
		.partitions = &vfe_partition,
		.npartitions = 1,
		.client_name = "vfe",
		.domain_flags = 0,
	};

	vfe_dev = kzalloc(sizeof(struct vfe_device), GFP_KERNEL);
	if (!vfe_dev) {
		pr_err("%s: no enough memory\n", __func__);
		return -ENOMEM;
	}

	if (pdev->dev.of_node) {
		of_property_read_u32((&pdev->dev)->of_node,
			"cell-index", &pdev->id);
		match_dev = of_match_device(msm_vfe_dt_match, &pdev->dev);
		vfe_dev->hw_info =
			(struct msm_vfe_hardware_info *) match_dev->data;
	} else {
		vfe_dev->hw_info = (struct msm_vfe_hardware_info *)
			platform_get_device_id(pdev)->driver_data;
	}

	if (!vfe_dev->hw_info) {
		pr_err("%s: No vfe hardware info\n", __func__);
		return -EINVAL;
	}
	ISP_DBG("%s: device id = %d\n", __func__, pdev->id);

	vfe_dev->pdev = pdev;
	rc = vfe_dev->hw_info->vfe_ops.core_ops.get_platform_data(vfe_dev);
	if (rc < 0) {
		pr_err("%s: failed to get platform resources\n", __func__);
		kfree(vfe_dev);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&vfe_dev->tasklet_q);
	tasklet_init(&vfe_dev->vfe_tasklet,
		msm_isp_do_tasklet, (unsigned long)vfe_dev);

	v4l2_subdev_init(&vfe_dev->subdev.sd, vfe_dev->hw_info->subdev_ops);
	vfe_dev->subdev.sd.internal_ops =
		vfe_dev->hw_info->subdev_internal_ops;
	snprintf(vfe_dev->subdev.sd.name,
		ARRAY_SIZE(vfe_dev->subdev.sd.name),
		"vfe");
	vfe_dev->subdev.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	vfe_dev->subdev.sd.flags |= V4L2_SUBDEV_FL_HAS_EVENTS;
	v4l2_set_subdevdata(&vfe_dev->subdev.sd, vfe_dev);
	platform_set_drvdata(pdev, &vfe_dev->subdev.sd);
	mutex_init(&vfe_dev->realtime_mutex);
	mutex_init(&vfe_dev->core_mutex);
	spin_lock_init(&vfe_dev->tasklet_lock);
	spin_lock_init(&vfe_dev->shared_data_lock);
	media_entity_init(&vfe_dev->subdev.sd.entity, 0, NULL, 0);
	vfe_dev->subdev.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	vfe_dev->subdev.sd.entity.group_id = MSM_CAMERA_SUBDEV_VFE;
	vfe_dev->subdev.sd.entity.name = pdev->name;
	vfe_dev->subdev.close_seq = MSM_SD_CLOSE_1ST_CATEGORY | 0x2;
	rc = msm_sd_register(&vfe_dev->subdev);
	if (rc != 0) {
		pr_err("%s: msm_sd_register error = %d\n", __func__, rc);
		kfree(vfe_dev);
		goto end;
	}

	msm_isp_v4l2_subdev_fops.owner = v4l2_subdev_fops.owner;
	msm_isp_v4l2_subdev_fops.open = v4l2_subdev_fops.open;
	msm_isp_v4l2_subdev_fops.release = v4l2_subdev_fops.release;
	msm_isp_v4l2_subdev_fops.poll = v4l2_subdev_fops.poll;

	vfe_dev->subdev.sd.devnode->fops = &msm_isp_v4l2_subdev_fops;

	vfe_dev->buf_mgr = &vfe_buf_mgr;
	v4l2_subdev_notify(&vfe_dev->subdev.sd,
		MSM_SD_NOTIFY_REQ_CB, &vfe_vb2_ops);
	rc = msm_isp_create_isp_buf_mgr(vfe_dev->buf_mgr,
		&vfe_vb2_ops, &vfe_layout);
	if (rc < 0) {
		pr_err("%s: Unable to create buffer manager\n", __func__);
		kfree(vfe_dev);
		return -EINVAL;
	}
	vfe_dev->buf_mgr->ops->register_ctx(vfe_dev->buf_mgr,
		&vfe_dev->iommu_ctx[0], vfe_dev->hw_info->num_iommu_ctx);
	vfe_dev->vfe_open_cnt = 0;
end:
	return rc;
}

static struct platform_driver vfe_driver = {
	.probe = vfe_probe,
	.driver = {
		.name = "msm_vfe",
		.owner = THIS_MODULE,
		.of_match_table = msm_vfe_dt_match,
	},
	.id_table = msm_vfe_dev_id,
};

static int __init msm_vfe_init_module(void)
{
	return platform_driver_register(&vfe_driver);
}

static void __exit msm_vfe_exit_module(void)
{
	platform_driver_unregister(&vfe_driver);
}

module_init(msm_vfe_init_module);
module_exit(msm_vfe_exit_module);
MODULE_DESCRIPTION("MSM VFE driver");
MODULE_LICENSE("GPL v2");
