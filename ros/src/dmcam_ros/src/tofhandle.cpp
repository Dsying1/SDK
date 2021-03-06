/*****************************************************************//**
 *       @file  tofhandle.c
 *      @brief  DM's camera device API
 *
 *  Detail Decsription starts here
 *
 *   @internal
 *     Project  $Project$
 *     Created  6/14/2017 
 *    Revision  $Id$
 *     Company  Data Miracle, Shanghai
 *   Copyright  (C) 2017 Data Miracle Intelligent Technologies
 *    
 *    THIS CODE AND INFORMATION ARE PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
 *    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
 *    PARTICULAR PURPOSE.
 *
 * *******************************************************************/
#include "tofhandle.h"
#include <pwd.h>
#include <unistd.h>
#define FILTER_ID_LEN_CALIB   1 //lens calibration
#define FILTER_ID_PIXEL_CALIB 0 //pixel calibration 
#define FILTER_ID_GAUSS       1 //Gauss filter for distance data
#define FILTER_ID_AMP         1 //Amplitude filter control
#define FILTER_ID_AUTO_INTG   0 //auto integration filter enable : use sat_ratio to adjust
#define FILTER_ID_SYNC_DELAY  0 //
#define FILTER_CNT            0 //
#define FILTER_ID_MEDIAN      1
#define FILTER_ID_HDR         0
#define FILTER_ID_FLYNOISE    0


#define EPC_DEVICE_635      "EPC635"
#define EPC_DEVICE_660      "EPC660"
#define SONY_DEVICE_IMX556  "IMX556"
#define PART_TYPE    0xFA
#define EE_DATA 0x12
#define EE_ADDR 0x11

typedef enum {
    EPC502_OBS = 1,
    EPC660 = 2,
    EPC502 = 3,
    EPC635 = 4,
    EPC503 = 5
}partType;


TofHandle::TofHandle(ros::NodeHandle *_n)
{
    m_nodeHandle = _n;
    m_cameraState = 0;
    m_imageTransport = new image_transport::ImageTransport((*_n));

    tofInitialize();
    if (m_cameraState) {
        topicInitialize();
        serverInitialize();
        m_rbgBuffer = new float[m_width*m_height];
        dist_pseudo_rgb = new uint8_t[m_curfsize*3];
        m_pclBuffer = new float[3 * m_curfsize];
    }
}

TofHandle::~TofHandle(void)
{
    if (m_cameraState) {
        free(m_frameBuffer);
        delete(m_rbgBuffer);
        delete(m_pclBuffer);
        delete(m_imageTransport);
        delete(dist_pseudo_rgb);
        dmcam_cap_stop(m_dev);
        dmcam_dev_close(m_dev);
        dmcam_uninit();
        ROS_INFO("[SMART TOF]smart tof is shutted down!");
    }
}

uint32_t TofHandle::epcGetPartType()
{
    uint32_t reg_val[1];
    reg_val[0]  = PART_TYPE;
    dmcam_reg_batch_write(m_dev,DEV_REG_CAM0,EE_ADDR, &reg_val[0], 1);
    dmcam_reg_batch_read(m_dev,DEV_REG_CAM0, EE_DATA, &reg_val[0], 1);
    ROS_INFO("epc:%d \n", reg_val[0]);
    return reg_val[0];
}

std::string TofHandle::checkDeviceType()
{
    char pname[16];
    char pid[16];
    char pver[16];

    std::string deviceType;
    dmcam_param_item_t rparam;
    memset(&rparam, 0, sizeof(rparam));
    rparam.param_id = PARAM_INFO_PRODUCT;
    dmcam_param_batch_get(m_dev, &rparam, 1);
    std::string product = rparam.param_val.info_product;
    if (3 != sscanf(product.c_str(), "%16[^-]-%16[^-]-%16[^-]", pname, pid, pver)) {
        return "";
    }

    //EPC device id:E1、E2、E3、E4，SONY device id：S1
    if (pid[0] == 'E')
    {
        int chip_type = epcGetPartType();
        if (chip_type == EPC635)
        {
            deviceType = EPC_DEVICE_635;
        }else if (chip_type == EPC660)
        {
            deviceType = EPC_DEVICE_660;

        }
    }
    else if (pid[0] == 'S')
    {
        deviceType = SONY_DEVICE_IMX556;
    }
    else
    {
        return "";
    }
    std::cout << "find device:"<< product.c_str() << std::endl;
    ROS_INFO("DeviceType=%s\n", product.c_str());
    return deviceType;
}


void TofHandle::getFrameInfo(std::string deviceType)
{
    dmcam_param_item_t rparam;
    memset(&rparam, 0, sizeof(rparam));
    rparam.param_id = PARAM_ROI;
    dmcam_param_batch_get(m_dev, &rparam, 1);
    dmcam_param_roi_t frameInfo = rparam.param_val.roi;
    m_height = frameInfo.erow - frameInfo.srow + 1;
    m_width = frameInfo.ecol - frameInfo.scol + 1;
    if (deviceType == EPC_DEVICE_660 )
    {
        m_width = 320;
        m_height = 240;
    }
    else if (deviceType == SONY_DEVICE_IMX556)
    {
        m_width = 640;
        m_height = 480;
    }
    m_frameSize = m_width*m_height*4*2;
    ROS_INFO("width=%d,height=%d,frame_size=%d\n", m_width, m_height, m_frameSize);
}


//initial tof
void TofHandle::tofInitialize(void)
{
    struct passwd *pwd;
    pwd = getpwuid(getuid());
    char dmcamLogName[100] = { 0 };
    char dmcamPath[100] = { 0 };
    sprintf(dmcamLogName,"/home/%s/tmp/dmcam.log",pwd->pw_name);
    sprintf(dmcamPath,"/home/%s/tmp/",pwd->pw_name);
    ROS_INFO("dmcamLogName is %s,dmcamPath is %s \n",dmcamLogName,dmcamPath);
    dmcam_init(dmcamLogName);
	dmcam_log_cfg(LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG, LOG_LEVEL_NONE);
    dmcam_path_set(dmcamPath);
    //try to open smart tof
    dmcam_dev_t dev_list[4];
    std::string oniFileName;
    m_nodeHandle->getParam("oni_file", oniFileName);
    ROS_INFO("[tofInitialize] open oni file name is %s\n", oniFileName.c_str());
    if(!oniFileName.empty())
    {
        m_dev = dmcam_dev_open_by_uri(oniFileName.c_str());
    }else
    {
        int dev_cnt = dmcam_dev_list(dev_list, 4);
        ROS_INFO("[SMART TOF]%d dmcam device found!", dev_cnt);
        if (dev_cnt == 0) {
            ROS_INFO("[SMART TOF]find device failed. Please kill the process and check your device.");
            return;
        }
    #if NEEDSUDO
        sudodev(&dev_list[0]);
    #endif

        m_dev = dmcam_dev_open(NULL);
        if (!m_dev) {
            ROS_INFO("[SMART TOF]open device failed!");
            return;
        } else {
            ROS_INFO("[SMART TOF]open device succeeded!");
        }
    }
    
    dmcam_cap_cfg_t cap_cfg;
    memset(&cap_cfg,0,sizeof(dmcam_cap_cfg_t));
    cap_cfg.en_fdev_rewind = true;
    cap_cfg.cache_frames_cnt = 10;
    dmcam_cap_config_set(m_dev, &cap_cfg);
    
    //init the device opened
    std::string deviceType = checkDeviceType();
    getFrameInfo(deviceType);
    paramInitialize(m_dev);

    m_frameBuffer = (uint8_t *)malloc(m_frameSize);
    m_curfsize= m_width*m_height*2;
    m_cameraState = 1;
    dmcam_cap_start(m_dev);
    ROS_INFO("[SMART TOF]curfsize=%d\n", m_curfsize);

    ROS_INFO("[SMART TOF]curfsize=%s\n", dmcam_path_get());
}

//initialize server
void TofHandle::serverInitialize(void)
{
    m_filterType.insert(std::make_pair("DMCAM_FILTER_ID_LEN_CALIB", DMCAM_FILTER_ID_LEN_CALIB));
    m_filterType.insert(std::make_pair("DMCAM_FILTER_ID_PIXEL_CALIB", DMCAM_FILTER_ID_PIXEL_CALIB));
    m_filterType.insert(std::make_pair("DMCAM_FILTER_ID_AMP", DMCAM_FILTER_ID_AMP));
    m_filterType.insert(std::make_pair("DMCAM_FILTER_ID_AUTO_INTG", DMCAM_FILTER_ID_AUTO_INTG));
    m_filterType.insert(std::make_pair("DMCAM_FILTER_ID_SYNC_DELAY", DMCAM_FILTER_ID_SYNC_DELAY));
    m_filterType.insert(std::make_pair("DMCAM_FILTER_CNT", DMCAM_FILTER_CNT));
    m_filterType.insert(std::make_pair("DMCAM_FILTER_ID_FLYNOISE", DMCAM_FILTER_ID_FLYNOISE));
    m_filterType.insert(std::make_pair("DMCAM_FILTER_ID_HDR", DMCAM_FILTER_ID_HDR));

    m_changePowerServer = m_nodeHandle->advertiseService("/smarttof/change_power", &TofHandle::changePower, this);
    m_changeIntgServer = m_nodeHandle->advertiseService("/smarttof/change_intg", &TofHandle::changeIntgTime, this);
    m_changeFrameServer = m_nodeHandle->advertiseService("/smarttof/change_frame_rate", &TofHandle::changeFrameRate, this);
    m_changeModfreqServer = m_nodeHandle->advertiseService("/smarttof/change_mod_freq", &TofHandle::changeModFreq, this);    
    m_changeSyncDelayServer = m_nodeHandle->advertiseService("/smarttof/change_sync_delay", &TofHandle::changeSyncDelay, this);
    m_changeFilterServer = m_nodeHandle->advertiseService("/smarttof/change_filter", &TofHandle::changeFilter, this);
    m_disableFilterServer = m_nodeHandle->advertiseService("/smarttof/disable_filter", &TofHandle::disableFilter, this);
}
//initial parameters
void TofHandle::paramInitialize(dmcam_dev_t *dev)
{
    
    bool flag = 0;
    int param_value[13];

    m_nodeHandle->getParam("testmode",        m_testMode);
    m_nodeHandle->getParam("dev_mode",        param_value[0]);
    m_nodeHandle->getParam("mod_freq",        param_value[1]);
    m_nodeHandle->getParam("format",          param_value[2]);
    m_nodeHandle->getParam("power_percent",   param_value[3]);
    m_nodeHandle->getParam("fps",             param_value[4]);
    m_nodeHandle->getParam("intg_us",         param_value[5]);
    m_nodeHandle->getParam("sync_delay",      param_value[6]);
    m_nodeHandle->getParam("enableflip",      param_value[7]);
    m_nodeHandle->getParam("flipcode",        param_value[8]);
    m_nodeHandle->getParam("srow",            param_value[9]);
    m_nodeHandle->getParam("erow",            param_value[10]);
    m_nodeHandle->getParam("scol",            param_value[11]);
    m_nodeHandle->getParam("ecol",            param_value[12]);
    m_devMode         = param_value[0];
    m_modFreq         = param_value[1];
    m_format          = param_value[2];
    power_percent     = param_value[3];
    m_fps             = param_value[4];
    m_intgTime        = param_value[5];
    m_syncDelay       = param_value[6];
    m_enbaleFlip      = param_value[7];
    m_flipCode        = param_value[8];
    m_roi.srow        = param_value[9];
    m_roi.erow        = param_value[10];
    m_roi.scol        = param_value[11];
    m_roi.ecol        = param_value[12];
    printf("----------setting params----------\n");
    dmcam_param_item_t param_items[7];
    param_items[0].param_id = PARAM_DEV_MODE;
    param_items[0].param_val.dev_mode = m_devMode;
    param_items[0].param_val_len = sizeof(m_devMode);

    memset(&param_items[1], 0, sizeof(param_items[2]));
    param_items[1].param_id = PARAM_INTG_TIME;
    param_items[1].param_val.intg.intg_us = m_intgTime;
    param_items[1].param_val_len = sizeof(m_intgTime);
    
    param_items[2].param_id = PARAM_FRAME_FORMAT;
    param_items[2].param_val.frame_format.format = m_format;
    param_items[2].param_val_len = sizeof(m_format);

    if(m_format==2 || m_format==0x0f){
        param_items[3].param_id = PARAM_MOD_FREQ;
        param_items[3].param_val.mod_freq = m_modFreq;
        param_items[3].param_val_len = sizeof(m_modFreq);
    }
    else if(m_format==0x11){
        param_items[3].param_id = PARAM_DUAL_MOD_FREQ;
        param_items[3].param_val.dual_freq.mod_freq0 = 54000000;
        param_items[3].param_val.dual_freq.mod_freq1 = 92000000;
        param_items[3].param_val_len = 2*sizeof(m_modFreq);
    }

    param_items[4].param_id = PARAM_FRAME_RATE;
    param_items[4].param_val.frame_rate.fps = m_fps;
    param_items[4].param_val_len = sizeof(m_fps);

    
    printf("param mode is %d\n", param_items[0].param_val.dev_mode);
    //printf("param power is %d\n", param_items[1].param_val.illum_power.percent);
    printf("param intg time is %d\n", param_items[1].param_val.intg.intg_us);
    printf("param format is %d\n", param_items[2].param_val.frame_format.format);
    //printf("param freq is %d\n", param_items[3].param_val.mod_freq);
    printf("param m_fps is %d\n", param_items[4].param_val.frame_rate.fps);
    for (int i = 0; i < 5; ++i) {
        flag = dmcam_param_batch_set(dev, &(param_items[i]), 1);
    }

    sleep(1);
    ROS_INFO("[SMART TOF]initialize parameters succeeds!");

    //camera intrinsic
    double tmp_param;
    cameraMatrix = cv::Mat::eye(3, 3, CV_64F);
    m_nodeHandle->getParam("cx", tmp_param);
    cam_int_param.cx = tmp_param;
    cameraMatrix.at<double>(0, 2) = tmp_param; //u
    m_nodeHandle->getParam("cy", tmp_param);
    cam_int_param.cy = tmp_param;
    cameraMatrix.at<double>(1, 2) = tmp_param;  //v
    m_nodeHandle->getParam("fx", tmp_param);
    cam_int_param.fx = tmp_param;
    cameraMatrix.at<double>(0, 0) = tmp_param;  //fx
    m_nodeHandle->getParam("fy", tmp_param);
    cam_int_param.fy = tmp_param;
    cameraMatrix.at<double>(1, 1) = tmp_param;  //fy
    intrinsicmat.cx = cam_int_param.cx;
    intrinsicmat.cy = cam_int_param.cy;
    intrinsicmat.fx = cam_int_param.fx;
    intrinsicmat.fy = cam_int_param.fy;
    intrinsicmat.scale = 1;

    XmlRpc::XmlRpcValue param_list;
    m_nodeHandle->getParam("dcoeff", param_list);
    distCoeffs = cv::Mat::zeros(5, 1, CV_64F);
    for(int i=0; i<param_list.size(); ++i){
        cam_int_param.dcoef[i] = double(param_list[i]);
        distCoeffs.at<double>(i, 0) = double(param_list[i]);
    }
    cv::initUndistortRectifyMap( cameraMatrix, distCoeffs, cv::Mat::eye(3,3,CV_64F), \
                                 cameraMatrix, cv::Size((m_roi.ecol-m_roi.scol), (m_roi.erow-m_roi.srow)), \
                                     CV_32F, map1, map2 );
    //camera info init
    m_cameraInfoMessage.height = m_height;
    m_cameraInfoMessage.width = m_width;
    m_cameraInfoMessage.roi.width = m_width;
    m_cameraInfoMessage.roi.height = m_height;
    m_cameraInfoMessage.distortion_model = sensor_msgs::distortion_models::PLUMB_BOB;
    m_cameraInfoMessage.header.frame_id = "dmcam_ros";
    m_cameraInfoMessage.K[2] = cam_int_param.cx;
    m_cameraInfoMessage.K[5] = cam_int_param.cy;
    m_cameraInfoMessage.K[0] = cam_int_param.fx;
    m_cameraInfoMessage.K[4] = cam_int_param.fy;
    m_cameraInfoMessage.K[8] = 1; //cam_int_param.scale
    m_cameraInfoMessage.R[0] = 1;
    m_cameraInfoMessage.R[4] = 1;
    m_cameraInfoMessage.R[8] = 1;
    m_cameraInfoMessage.P[2] = cam_int_param.cx;
    m_cameraInfoMessage.P[6] = cam_int_param.cy;
    m_cameraInfoMessage.P[0] = cam_int_param.fx;
    m_cameraInfoMessage.P[5] = cam_int_param.fy;
    m_cameraInfoMessage.P[10] = 1; //cam_int_param.scale
    m_imgHeadMsg.frame_id = "dmcam_ros";

    if (m_testMode) {
        //test part
        printf("-------get param after set-------\n");
        flag = 0;
        dmcam_param_item_t get_param1;
        get_param1.param_id = PARAM_ROI;
        flag = dmcam_param_batch_get(dev, &get_param1, 1);
        printf("get flag is %d,parameter len we get %d\n", flag, get_param1.param_val_len); //return 4
        get_param1.param_id = PARAM_MOD_FREQ;
        flag = dmcam_param_batch_get(dev, &get_param1, 1);
        printf("[%d]get flag is %d, parameter len we get %d, ", PARAM_MOD_FREQ, flag, get_param1.param_val_len);
        printf("get freq is %d\n", get_param1.param_val.mod_freq);

        get_param1.param_id = PARAM_ILLUM_POWER;
        flag = dmcam_param_batch_get(dev, &get_param1, 1);
        printf("[%d]get flag is %d, parameter len we get %d, ", PARAM_ILLUM_POWER, flag, get_param1.param_val_len);
        printf("get power is %d\n", get_param1.param_val.illum_power.percent);

        get_param1.param_id = PARAM_FRAME_RATE;
        flag = dmcam_param_batch_get(dev, &get_param1, 1);
        printf("[%d]get flag is %d, parameter len we get %d, ", PARAM_FRAME_RATE, flag, get_param1.param_val_len);
        printf("get m_fps is %d\n", get_param1.param_val.frame_rate.fps);

        get_param1.param_id = PARAM_INTG_TIME;
        flag = dmcam_param_batch_get(dev, &get_param1, 1);
        printf("[%d]get flag is %d, parameter len we get %d, ", PARAM_INTG_TIME, flag, get_param1.param_val_len);
        printf("get intg time is %d\n", get_param1.param_val.intg.intg_us);
    }
        /*filter*/
    dmcam_filter_args_u filter_args;

#if     FILTER_ID_LEN_CALIB
    dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_LEN_CALIB, &filter_args, sizeof(filter_args));
#endif

#if     FILTER_ID_PIXEL_CALIB
    dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_PIXEL_CALIB, &filter_args, sizeof(filter_args));
#endif

#if     FILTER_ID_AMP
    filter_args.min_amp = 30;
    dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_AMP, &filter_args, sizeof(filter_args));
#endif

#if FILTER_ID_MEDIAN
    filter_args.median_ksize = 100;
    dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_MEDIAN, &filter_args, sizeof(filter_args));
#endif

#if     FILTER_ID_AUTO_INTG
    dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_AUTO_INTG, &filter_args, sizeof(filter_args));
#endif

#if     FILTER_ID_SYNC_DELAY
    dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_SYNC_DELAY, &filter_args, sizeof(filter_args));
#endif

#if     FILTER_CNT
    dmcam_filter_enable(m_dev, DMCAM_FILTER_CNT, &filter_args, sizeof(filter_args));
#endif

#if     FILTER_ID_FLYNOISE
    filter_args.fly_noise_threshold = 20;
    dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_FLYNOISE, &filter_args, sizeof(filter_args));
#endif

#if     FILTER_ID_HDR
    if(m_format==0x0f){
        filter_args.intg.intg_3d = 100;     //HDR 模式时小的曝光时间
        filter_args.intg.intg_3dhdr = 900;  //HDR 模式时大的曝光时间
        dmcam_filter_enable(m_dev,DMCAM_FILTER_ID_HDR,&filter_args,sizeof(filter_args)); //开启HDR模式
    }
#endif            
}

//initial topics
void TofHandle::topicInitialize(void)
{
    m_imageGrayPub = m_imageTransport->advertise("/smarttof/image_gray", 1);
    m_imageDistPub = m_imageTransport->advertise("/smarttof/image_dist", 1);
    m_cameraInfoPublic = m_nodeHandle->advertise<sensor_msgs::CameraInfo>("/smarttof/camera_info", 1);
    m_pointCloudPublic = m_nodeHandle->advertise<sensor_msgs::PointCloud2>("smarttof/pointcloud", 1);
}

//sudo device
void TofHandle::sudodev(dmcam_dev_t *_dev)
{
    if (_dev == NULL)
        return;

    char path[21];
    char *_pa = "/dev/bus/usb/000/000";
    strcpy(path, _pa);
    int tmp = _dev->if_info.info.usb.usb_dev_addr;
    for (int i = 0; tmp != 0; ++i) {
        path[19 - i] = tmp % 10 - 0 + '0';
        tmp = tmp / 10;
    }
    path[15] = _dev->if_info.info.usb.usb_bus_num - 0 + '0';
    char *orhead = "sudo -S chmod 666 ";
    char order[39];
    strcpy(order, orhead);
    strcpy(&order[18], path);
    printf("%s\n", order);
    system(order);
}

//publish all topics
void TofHandle::publicAll(void)
{
    getOneFrame();
    publicImage();
    publicCaminfo();
}

//get tof state(can m_imageTransport be used?)
bool TofHandle::getTofState(void)
{
    return m_cameraState;
}

//get test mode state
bool TofHandle::getTestMode(void)
{
    return m_testMode;
}

//get one frame
void TofHandle::getOneFrame(void)
{
    if (1 != dmcam_cap_get_frames(m_dev, 1, m_frameBuffer, m_frameSize, &m_frameInfo)) {
        printf("Get frame failed:%d\n", m_curfsize * 4);
    }
}

//pub image
void TofHandle::publicImage(void) //pub_image in SDK1.72
{
    /*prepare header information*/
    m_imgHeadMsg.stamp     = ros::Time::now();

    int pix_cnt = dmcam_frame_get_distance(m_dev, m_rbgBuffer, m_width * m_height, m_frameBuffer, m_frameInfo.frame_info.frame_size, &m_frameInfo.frame_info);
    if (pix_cnt != m_height * m_width) 
    {
        printf(" unmatch pixcnt: %d, HxW: %dx%d\n", pix_cnt, m_height, m_width);
    }
    //publish point cloud
    publicPointCloud(m_rbgBuffer);

    const dmcam_lens_calib_cfg_t* cfg = dmcam_lens_calib_config_get(m_dev);
    if (cfg->en_2d_calib)
    {
        int dst_len = m_width * m_height;
        float *m_rbgBufferTmp = new float[m_width*m_height];
        memcpy(m_rbgBufferTmp, m_rbgBuffer, sizeof(float)*m_width*m_height);
        dmcam_lens_calib_apply_dist_f32(m_dev, m_rbgBuffer, dst_len, (const float *)m_rbgBufferTmp, dst_len, &m_frameInfo.frame_info, NULL);
        delete(m_rbgBufferTmp);
    }
    cv::Mat img_dist(m_height, m_width, CV_32FC1, m_rbgBuffer);
    cv::Mat img_dist_rectified = cv::Mat::zeros(m_height, m_width,CV_32FC1);
    if (m_enbaleFlip)cv::flip(img_dist, img_dist, m_flipCode);
    if (pix_cnt != m_height * m_width)
    {
        printf(" unmatch pixcnt: %d, HxW: %dx%d\n", pix_cnt, m_height, m_width);
    }
    //publish image dist
    cv_bridge::CvImage img_bridge;
    sensor_msgs::Image img_msg; // message to be sent
    // convert dist to pseudo img 
    dmcam_cmap_dist_f32_to_RGB(dist_pseudo_rgb, m_curfsize  * 3, m_rbgBuffer, pix_cnt, DMCAM_CMAP_OUTFMT_RGB, 0.0, 5.0,NULL);
    cv::Mat img_dist_rgb(m_height, m_width, CV_8UC3, dist_pseudo_rgb);
    cv::Mat img_dist_rgb_rect;
    img_dist_rgb_rect=img_dist_rgb.clone();
    img_bridge = cv_bridge::CvImage(m_imgHeadMsg, sensor_msgs::image_encodings::RGB8, img_dist_rgb_rect);
    img_bridge.toImageMsg(img_msg);
    m_imageDistPub.publish(img_msg);

    // publish image_gray
    dmcam_frame_get_gray(m_dev, m_rbgBuffer, m_width * m_height, m_frameBuffer, m_frameInfo.frame_info.frame_size, &m_frameInfo.frame_info);
    
    if (cfg->en_2d_calib)
    {
        int gray_len = m_width * m_height;
        float *m_rbgBufferTmp = new float[m_width*m_height];
        memcpy(m_rbgBufferTmp, m_rbgBuffer, sizeof(float)*m_width*m_height);
        dmcam_lens_calib_apply_gray_f32(m_dev, m_rbgBuffer, gray_len, (const float *)m_rbgBufferTmp, gray_len, &m_frameInfo.frame_info, NULL);
        delete(m_rbgBufferTmp);
    }
    cv::Mat img_gray(m_height, m_width, CV_32FC1, m_rbgBuffer);
    //image flip code  0 : 垂直翻转, 1 : 水平翻转, -1 : 水平垂直翻转转
    if (m_enbaleFlip)cv::flip(img_gray, img_gray, m_flipCode);
    img_gray.convertTo(img_gray,CV_8UC1);
    cv::Mat img_gray_rectified;
    img_gray_rectified=img_gray.clone();
    if(m_testMode)
    {
        cv::imshow("img gray", img_gray_rectified);
    }
    m_imageGrayPub.publish(sensor_msgs::ImagePtr(cv_bridge::CvImage(m_imgHeadMsg, "mono8", img_gray_rectified).toImageMsg()));

    if (m_testMode) {
        cv::waitKey(1);
        cv::moveWindow("img gray", 1, 480);
        cv::moveWindow("img dist", 1, 480);
    }
}

//publish point cloud
void TofHandle::publicPointCloud(float* pcl_buff)
{
    dmcam_frame_get_pcl(m_dev,m_pclBuffer,3 * m_curfsize/2,pcl_buff,m_frameInfo.frame_info.frame_size,m_frameInfo.frame_info.width,m_frameInfo.frame_info.height,NULL);
    pclPointCloudXYZ::Ptr pCloud(new pclPointCloudXYZ);
    for (int m = 0; m < m_curfsize/2; m++) {
        pCloud->points.push_back(pcl::PointXYZ(m_pclBuffer[3 * m], m_pclBuffer[3 * m + 1], m_pclBuffer[3 * m + 2]));
    }
    pCloud->height = 1;
    pCloud->width = pCloud->points.size();
    pCloud->is_dense = false;
    pcl::toROSMsg(*pCloud, m_pointCloudMsg);
    m_pointCloudMsg.header.frame_id = "dmcam_ros";
    m_pointCloudMsg.header.stamp = ros::Time::now();
    m_pointCloudMsg.is_dense = false;
    m_pointCloudPublic.publish(m_pointCloudMsg);
    pCloud->points.clear();
}

//publish camera info
void TofHandle::publicCaminfo(void)
{
    m_cameraInfoMessage.header.stamp = ros::Time::now();
    m_cameraInfoPublic.publish(m_cameraInfoMessage);
}

//capture error handle function
static bool on_cap_err(dmcam_dev_t *_dev, int _err, void *_err_arg)
{
    ROS_INFO("[SMART TOF]cap error found : %s, arg=%p!", dmcam_error_name(_err), _err_arg);
    switch (_err) {
        case DMCAM_ERR_CAP_FRAME_DISCARD:
            ROS_INFO("[SMART TOF]data process too slow: total missing %d frames\n", *((uint32_t *)&_err_arg));
            break;
        case DMCAM_ERR_CAP_STALL:
            ROS_INFO("[SMART TOF]usb pipe stall!\n");
            break;
        default:
            break;
    }
    dmcam_cap_stop(_dev);
    dmcam_cap_start(_dev);
    return true;
}

bool TofHandle::changeParameters(dmcam_param_item_t params)
{    
    bool flag = 0;
    switch (params.param_id)
        {
            case PARAM_ILLUM_POWER:
                printf("change_parameters power_percent is %d\n", params.param_val.illum_power.percent);
                break;
            case PARAM_INTG_TIME:
                printf("change_parameters m_intgTime is %d\n", params.param_val.intg.intg_us);
                break;
            case PARAM_FRAME_RATE:
                printf("change_parameters m_fps is %d\n", params.param_val.frame_rate.fps);
                break;
            case PARAM_MOD_FREQ:
                printf("change_parameters mod_freq is %d\n", params.param_val.mod_freq);
                break;
            case PARAM_DUAL_MOD_FREQ:
                printf("change_parameters mod_freq1 is %d; mod_freq2 is %d\n", params.param_val.dual_freq.mod_freq0,params.param_val.dual_freq.mod_freq1);
                break;
            case PARAM_SYNC_DELAY:
                printf("change_parameters sync_delay is %d\n", params.param_val.sync_delay.delay);
                break;
            default:
                break;
        }
    flag = dmcam_param_batch_set(m_dev, &params, 1);
    dmcam_param_item_t param;
    param.param_id = PARAM_INTG_TIME;
    param.param_val_len = sizeof(param);
    dmcam_param_batch_get(m_dev, &param, 1);
 
    printf("dmcam_param_batch_get m_intgTime is %d\n", param.param_val.intg.intg_us);
    return flag;
}

bool TofHandle::changePower(dmcam_ros::change_power::Request& req, dmcam_ros::change_power::Response& res)
{
    power_percent = req.power_value;
    res.power_percent_new = power_percent;
    dmcam_param_item_t reset_power;
    memset(&reset_power, 0, sizeof(reset_power));
    reset_power.param_id = PARAM_ILLUM_POWER;
    reset_power.param_val.illum_power.percent = power_percent;
    reset_power.param_val_len = sizeof(power_percent);
    return changeParameters(reset_power);
}

bool TofHandle::changeIntgTime(dmcam_ros::change_intg::Request& req, dmcam_ros::change_intg::Response& res)
{
    m_intgTime = req.intg_value;
    res.intg_us_new = m_intgTime;
    dmcam_param_item_t reset_intg;
    memset(&reset_intg, 0, sizeof(reset_intg));
    reset_intg.param_id = PARAM_INTG_TIME;
    reset_intg.param_val.intg.intg_us = m_intgTime;
    reset_intg.param_val_len = sizeof(m_intgTime);
    printf("changeIntgTime m_intgTime is %d\n", m_intgTime);
    return changeParameters(reset_intg);
}

bool TofHandle::changeFrameRate(dmcam_ros::change_frame_rate::Request& req, dmcam_ros::change_frame_rate::Response& res)
{
    m_fps = req.frame_rate_value;
    res.frame_rate_new_value = m_fps;
    dmcam_param_item_t reset_frame_rate;
    memset(&reset_frame_rate, 0, sizeof(reset_frame_rate));
    reset_frame_rate.param_id = PARAM_FRAME_RATE;
    reset_frame_rate.param_val.frame_rate.fps = m_fps;
    reset_frame_rate.param_val_len = sizeof(m_fps);
    return changeParameters(reset_frame_rate);
}

bool TofHandle::changeModFreq(dmcam_ros::change_mod_freq::Request& req, dmcam_ros::change_mod_freq::Response& res)
{
    m_modFreq = req.mod_freq_value;
    m_modFreq1 = req.mod_freq_value1;
    dmcam_param_item_t reset_mod_freq;
    memset(&reset_mod_freq, 0, sizeof(reset_mod_freq));
    if(m_modFreq1==0){
        std::cout<<"change_filter_id is changeModFreq";
        dmcam_param_item_t wparam;
        memset(&wparam,0,sizeof(wparam));
        wparam.param_id = PARAM_FRAME_FORMAT;
        wparam.param_val.frame_format.format = 2;
        wparam.param_val_len = sizeof(m_format);
        dmcam_param_batch_set(m_dev,&wparam,1);
        reset_mod_freq.param_id = PARAM_MOD_FREQ;
        reset_mod_freq.param_val.mod_freq = m_modFreq;
        reset_mod_freq.param_val_len = sizeof(m_modFreq);
    }
    else{
        std::cout<<"change_filter_id is change dual ModFreq";
        dmcam_param_item_t wparam;
        memset(&wparam,0,sizeof(wparam));
        wparam.param_id = PARAM_FRAME_FORMAT;
        wparam.param_val.frame_format.format = 0x11;
        wparam.param_val_len = sizeof(m_format);
        dmcam_param_batch_set(m_dev,&wparam,1);
        reset_mod_freq.param_id = PARAM_DUAL_MOD_FREQ;
        reset_mod_freq.param_val.dual_freq.mod_freq0 = m_modFreq;
        reset_mod_freq.param_val.dual_freq.mod_freq1 = m_modFreq1;
        reset_mod_freq.param_val_len = 2*sizeof(m_modFreq);
    }
    res.mod_freq_new_value = m_modFreq; res.mod_freq_new_value1 = m_modFreq1;
    return changeParameters(reset_mod_freq);
}
bool TofHandle::changeSyncDelay(dmcam_ros::change_sync_delay::Request& req, dmcam_ros::change_sync_delay::Response& res)
{
    m_syncDelay = req.sync_delay_value;
    res.sync_delay_new_value = m_syncDelay;
    dmcam_param_item_t reset_sync_delay;
    memset(&reset_sync_delay, 0, sizeof(reset_sync_delay));
    reset_sync_delay.param_id = PARAM_SYNC_DELAY;
    reset_sync_delay.param_val.sync_delay.delay = m_syncDelay;
    reset_sync_delay.param_val_len = sizeof(m_syncDelay);
    return changeParameters(reset_sync_delay);
}

bool TofHandle::changeFilter(dmcam_ros::change_filter::Request& req, dmcam_ros::change_filter::Response& res)
{
    int filter_value =  0;
    filter_value = req.filter_value;
    printf("change_filter_id is %s\n",req.filter_id.c_str());
    printf("change_filter_value is %d\n",req.filter_value);
    dmcam_filter_args_u filter_args;
    dmcam_filter_id_e filter_type;
    std::map<std::string, dmcam_filter_id_e>::iterator itor;
    for (itor = m_filterType.begin(); itor != m_filterType.end(); itor++){
        if (itor->first == req.filter_id){
            filter_type = itor->second;
        }
    }
    switch (filter_type)
    {
    case DMCAM_FILTER_ID_LEN_CALIB:
        std::cout<<"change_filter_id is DMCAM_FILTER_ID_LEN_CALIB";
        dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_LEN_CALIB, &filter_args, sizeof(filter_args));
        break;
    case DMCAM_FILTER_ID_PIXEL_CALIB:
        dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_PIXEL_CALIB, &filter_args, sizeof(filter_args));
        break;
    case DMCAM_FILTER_ID_AMP:
        if(filter_value == 0)
        {
            filter_value = 30;
        }
        filter_args.min_amp = filter_value;
        dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_AMP, &filter_args, sizeof(filter_args));
        break;
    case DMCAM_FILTER_ID_AUTO_INTG:
        dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_AUTO_INTG, &filter_args, sizeof(filter_args));
        break;
    case DMCAM_FILTER_ID_SYNC_DELAY:
        dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_SYNC_DELAY, &filter_args, sizeof(filter_args));
        break;
    case DMCAM_FILTER_CNT:
        dmcam_filter_enable(m_dev, DMCAM_FILTER_CNT, &filter_args, sizeof(filter_args));
        break;
    case DMCAM_FILTER_ID_HDR:
        printf("change_filter_id is 1117");
        dmcam_param_item_t wparam;
        memset(&wparam,0,sizeof(wparam));
        wparam.param_id = PARAM_FRAME_FORMAT;
        wparam.param_val.frame_format.format = 0x0f;
        wparam.param_val_len = sizeof(m_format);
        dmcam_param_batch_set(m_dev,&wparam,1);
        filter_args.intg.intg_3d = m_intgTime;        //HDR 模式时小的曝光时间
        filter_args.intg.intg_3dhdr = filter_value;  //HDR 模式时大的曝光时间
        dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_HDR, &filter_args, sizeof(filter_args)); //开启HDR模式
        break;
    case DMCAM_FILTER_ID_FLYNOISE:
        filter_args.fly_noise_threshold = filter_value;
        dmcam_filter_enable(m_dev, DMCAM_FILTER_ID_FLYNOISE, &filter_args, sizeof(filter_args)); //start flynoise filter
        break;
    default:
        break;
    }
    std::cout<<"change_filter_id is 2222"<<std::endl;
    res.change_filter_id = req.filter_id.c_str();
    res.change_filter_value = req.filter_value;
}

void TofHandle::undistort( cv::InputArray _src, cv::OutputArray _dst, cv::InputArray _cameraMatrix,
                cv::InputArray _distCoeffs, cv::InputArray _newCameraMatrix )
{
    cv::Mat src = _src.getMat(), cameraMatrix = _cameraMatrix.getMat();
    cv::Mat distCoeffs = _distCoeffs.getMat(), newCameraMatrix = _newCameraMatrix.getMat();

    _dst.create( src.size(), src.type() );
    cv::Mat dst = _dst.getMat();

    CV_Assert( dst.data != src.data );

    int stripe_size0 = std::min(std::max(1, (1 << 12) / std::max(src.cols, 1)), src.rows);
    cv::Mat map1(stripe_size0, src.cols, CV_16SC2), map2(stripe_size0, src.cols, CV_16UC1);

    cv::Mat_<double> A, Ar, I = cv::Mat_<double>::eye(3,3);

    cameraMatrix.convertTo(A, CV_64F);
    if( !distCoeffs.empty() )
        distCoeffs = cv::Mat_<double>(distCoeffs);
    else
    {
        distCoeffs.create(5, 1, CV_64F);
        distCoeffs = 0.;
    }

    if( !newCameraMatrix.empty() )
        newCameraMatrix.convertTo(Ar, CV_64F);
    else
        A.copyTo(Ar);

    double v0 = Ar(1, 2);
    for( int y = 0; y < src.rows; y += stripe_size0 )
    {
        int stripe_size = std::min( stripe_size0, src.rows - y );
        Ar(1, 2) = v0 - y;
        cv::Mat map1_part = map1.rowRange(0, stripe_size),
            map2_part = map2.rowRange(0, stripe_size),
            dst_part = dst.rowRange(y, y + stripe_size);

        cv::initUndistortRectifyMap( A, distCoeffs, I, Ar, cv::Size(src.cols, stripe_size),
                                 map1_part.type(), map1_part, map2_part );
        cv::remap( src, dst_part, map1_part, map2_part, cv::INTER_NEAREST, cv::BORDER_REPLICATE );
    }
}

bool TofHandle::disableFilter(dmcam_ros::disable_filter::Request& req, dmcam_ros::disable_filter::Response& res)
{
    printf("disable_filter is %s\n",req.filter_id.c_str());
    dmcam_filter_id_e filter_type;
    std::map<std::string, dmcam_filter_id_e>::iterator itor;
    for (itor = m_filterType.begin(); itor != m_filterType.end(); itor++){
        if (itor->first == req.filter_id){
            filter_type = itor->second;
        }
    }
    switch (filter_type)
    {    
    case DMCAM_FILTER_ID_LEN_CALIB:
        dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_LEN_CALIB);
        break;
    case DMCAM_FILTER_ID_PIXEL_CALIB:
        dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_PIXEL_CALIB);
        break;
    case DMCAM_FILTER_ID_AMP:
        dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_AMP);
        break;
    case DMCAM_FILTER_ID_AUTO_INTG:
        dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_AUTO_INTG);    
        break;
    case DMCAM_FILTER_ID_SYNC_DELAY:
        dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_SYNC_DELAY);
        break;
    case DMCAM_FILTER_CNT:
        dmcam_filter_disable(m_dev, DMCAM_FILTER_CNT);
        break;
    case DMCAM_FILTER_ID_HDR:
        dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_HDR); //close HDR
        break;
    case DMCAM_FILTER_ID_FLYNOISE:
        //int a = dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_FLYNOISE); //stop flynoise filter
        std::cout<<dmcam_filter_disable(m_dev, DMCAM_FILTER_ID_FLYNOISE)<<std::endl;
        break;
    default:
        break;
    }
    res.disable_filter_id = req.filter_id.c_str();
}
