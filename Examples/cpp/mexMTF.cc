#include "mtf/mtf.h"
//! tools for reading in images from various sources like image sequences, 
//! videos and cameras as well as for pre processing them
#include "mtf/pipeline.h"
#include "mtf/Config/parameters.h"
#include "mtf/Utilities/miscUtils.h"

#include <time.h>
#include <string.h>
#include <vector>
#include <map>
#include <memory>

#include "opencv2/core/core.hpp"
#include <mex.h>

#define MEX_TRACKER_CREATE 1
#define MEX_TRACKER_INITIALIZE 2
#define MEX_TRACKER_UPDATE 3
#define MEX_TRACKER_SET_REGION 4
#define MEX_TRACKER_REMOVE 5

#define _A3D_IDX_COLUMN_MAJOR(i,j,k,nrows,ncols) ((i)+((j)+(k)*ncols)*nrows)
// interleaved row-major indexing for 2-D OpenCV images
//#define _A3D_IDX_OPENCV(x,y,c,mat) (((y)*mat.step[0]) + ((x)*mat.step[1]) + (c))
#define _A3D_IDX_OPENCV(i,j,k,nrows,ncols,nchannels) (((i*ncols + j)*nchannels) + (k))

using namespace std;
using namespace mtf::params;
typedef unique_ptr<mtf::TrackerBase> Tracker_;

static std::map<std::string, const int> cmd_list = {
	{ "create", MEX_TRACKER_CREATE },
	{ "initialize", MEX_TRACKER_INITIALIZE },
	{ "update", MEX_TRACKER_UPDATE },
	{ "set_region", MEX_TRACKER_SET_REGION },
	{ "remove", MEX_TRACKER_REMOVE }
};

static std::vector<Tracker_> trackers;
static std::vector<PreProc_> pre_procs;

static double min_x, min_y, max_x, max_y;
static double size_x, size_y;

static int img_height, img_width;
static vector<cv::Scalar> obj_cols;

static int frame_id;
static unsigned int tracker_id = 0;
static char* config_root_dir = nullptr;
static bool using_input_pipeline = false;

static bool tracker_created = false, tracker_initialized = false;

bool createTracker() {
	if(!config_root_dir){
		config_root_dir = "../../Config";
		printf("Using default configuration folder: %s\n", config_root_dir);
	} else{
		printf("Reading MTF configuration files from: %s\n", config_root_dir);
	}
	config_dir = std::string(config_root_dir);
	if(!readParams(0, nullptr)){ return false; }

	try{
		trackers.push_back(Tracker_(mtf::getTracker(mtf_sm, mtf_am, mtf_ssm, mtf_ilm)));
		if(!trackers.back()){
			printf("Tracker could not be created successfully\n");
			return false;
		}
	} catch(const mtf::utils::Exception &err){
		printf("Exception of type %s encountered while creating the tracker: %s\n",
			err.type(), err.what());
		return false;
	}
	try{
		pre_procs.push_back(mtf::getPreProc(trackers.back()->inputType(), pre_proc_type));
	} catch(const mtf::utils::Exception &err){
		printf("Exception of type %s encountered while creating the pre processor: %s\n",
			err.type(), err.what());
		return false;
	}
	return true;
}

bool initializeTracker(const cv::Mat &init_img, const cv::Mat &init_corners) {
	img_height = init_img.rows;
	img_width = init_img.cols;

	printf("img_height: %d\n", img_height);
	printf("img_width: %d\n", img_width);

	printf("init_corners:\n");
	for(unsigned int corner_id = 0; corner_id < 4; ++corner_id) {
		printf("%d: (%f, %f)\n", corner_id, init_corners.at<double>(0, corner_id), init_corners.at<double>(1, corner_id));
	}
	min_x = init_corners.at<double>(0, 0);
	min_y = init_corners.at<double>(1, 0);
	max_x = init_corners.at<double>(0, 2);
	max_y = init_corners.at<double>(1, 2);
	size_x = max_x - min_x;
	size_y = max_y - min_y;
	try{
		pre_procs[tracker_id]->initialize(init_img);
	} catch(const mtf::utils::Exception &err){
		printf("Exception of type %s encountered while initializing the pre processor: %s\n",
			err.type(), err.what());
		return false;
	}
	try{
		for(PreProc_ curr_obj = pre_procs[tracker_id]; curr_obj; curr_obj = curr_obj->next){
			trackers[tracker_id]->setImage(curr_obj->getFrame());
		}
		printf("Initializing tracker with object of size %f x %f\n", size_x, size_y);
		trackers[tracker_id]->initialize(init_corners);
	} catch(const mtf::utils::Exception &err){
		printf("Exception of type %s encountered while initializing the tracker: %s\n",
			err.type(), err.what());
		return false;
	}
	frame_id = 0;
	return true;
}

bool updateTracker(const cv::Mat &curr_img) {
	double fps = 0, fps_win = 0;
	double tracking_time, tracking_time_with_input;
	++frame_id;
	mtf_clock_get(start_time_with_input);
	try{
		//! update pre processor
		pre_procs[tracker_id]->update(curr_img);
		mtf_clock_get(start_time);
		//! update tracker
		trackers[tracker_id]->update();
		if(print_fps){
			mtf_clock_get(end_time);
			mtf_clock_measure(start_time, end_time, tracking_time);
			mtf_clock_measure(start_time_with_input, end_time, tracking_time_with_input);
			fps = 1.0 / tracking_time;
			fps_win = 1.0 / tracking_time_with_input;
			printf("fps: %f\t fps_win=%f\n", fps, fps_win);
		}
		if(reset_template){
			trackers[tracker_id]->initialize(trackers[tracker_id]->getRegion());
		}
	} catch(const mtf::utils::Exception &err){
		printf("Exception of type %s encountered while updating the tracker: %s\n",
			err.type(), err.what());
		return false;
	}
	return true;
}
bool setRegion(const cv::Mat &corners) {
	try{
		trackers[tracker_id]->setRegion(corners);
	} catch(const mtf::utils::Exception &err){
		printf("Exception of type %s encountered while resetting the tracker: %s\n",
			err.type(), err.what());
		return false;
	}
	return true;
}
/**
* Copy the (image) data from Matlab-algorithm compatible (column-major) representation to cv::Mat.
* The information about the image are taken from the OpenCV cv::Mat structure.
adapted from OpenCV-Matlab package available at: https://se.mathworks.com/matlabcentral/fileexchange/41530-opencv-matlab
*/
template <typename T>
inline void
copyMatrixFromMatlab(const T* from, cv::Mat& to, int n_channels){

	const int n_rows = to.rows;
	const int n_cols = to.cols;

	T* pdata = (T*)to.data;

	for(int c = 0; c < n_channels; ++c){
		for(int x = 0; x < n_cols; ++x){
			for(int y = 0; y < n_rows; ++y){
				const T element = from[_A3D_IDX_COLUMN_MAJOR(y, x, c, n_rows, n_cols)];
				pdata[_A3D_IDX_OPENCV(y, x, c, rows, n_cols, n_channels)] = element;
			}
		}
	}
}
unsigned int getTrackerID(const mxArray *mx_tracker_id){
	if(!mxIsClass(mx_tracker_id, "uint32")){
		mexErrMsgTxt("Tracker ID must be of 32 bit unsigned integral type");
	}
	unsigned int* tracker_id_ptr = (unsigned int*)mxGetData(mx_tracker_id);
	return *tracker_id_ptr;
}
cv::Mat getImage(const mxArray *mx_img){
	int img_n_dims = mxGetNumberOfDimensions(mx_img);
	if(!mxIsClass(mx_img, "uint8")){
		mexErrMsgTxt("Input image must be of 8 bit unsigned integral type");
	}
	if(img_n_dims < 2 || img_n_dims > 3){
		mexErrMsgTxt("Input image must have 2 or 3 dimensions");
	}
	int img_type = img_n_dims == 2 ? CV_8UC1 : CV_8UC3;
	const mwSize *img_dims = mxGetDimensions(mx_img);
	int height = img_dims[0];
	int width = img_dims[1];
	//printf("width: %d\t height=%d\t img_n_dims: %d\n", width, height, img_n_dims);
	unsigned char *img_ptr = (unsigned char*)mxGetData(mx_img);
	cv::Mat img(height, width, img_type);
	if(img_n_dims == 2){
		cv::Mat img_transpose(width, height, img_type, img_ptr);
		cv::transpose(img_transpose, img);
	} else{
		copyMatrixFromMatlab(img_ptr, img, 3);
	}
	return img;
}
cv::Mat getCorners(const mxArray *mx_corners){
	int corners_n_dims = mxGetNumberOfDimensions(mx_corners);
	if(!mxIsClass(mx_corners, "double")){
		mexErrMsgTxt("Input corner array must be of 64 bit floating point type");
	}
	if(corners_n_dims != 2){
		mexErrMsgTxt("Input corner array must have 2 dimensions");
	}
	const mwSize *corners_dims = mxGetDimensions(mx_corners);
	if(corners_dims[0] != 2 || corners_dims[1] != 4){
		mexErrMsgTxt("Input corner array must be of size 2 x 4");
	}
	double *corners_ptr = mxGetPr(mx_corners);
	cv::Mat corners_transposed(4, 2, CV_64FC1, corners_ptr);
	cout << "corners_transposed: \n" << corners_transposed << "\n";
	cv::Mat corners(2, 4, CV_64FC1);
	cv::transpose(corners_transposed, corners);
	cout << "corners: \n" << corners << "\n";
	return corners;
}
mxArray *setCorners(const cv::Mat &corners){
	mxArray *mx_corners = mxCreateDoubleMatrix(2, 4, mxREAL);
	/* get a pointer to the real data in the output matrix */
	double *out_corners_ptr = mxGetPr(mx_corners);
	cv::Mat out_corners(4, 2, CV_64FC1, out_corners_ptr);
	cv::transpose(corners, out_corners);
	return mx_corners;
}
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]){
	plhs[0] = mxCreateDoubleMatrix(1, 1, mxREAL);
	double *ret_val = mxGetPr(plhs[0]);
	if(nrhs < 1){
		mexErrMsgTxt("Not enough input arguments.");
	}
	if(!mxIsChar(prhs[0])){
		mexErrMsgTxt("The first argument must be a string.");
	}
	int cmd_str_len = mxGetM(prhs[0])*mxGetN(prhs[0]) + 1;
	char *cmd_str = (char *)mxMalloc(cmd_str_len);
	mxGetString(prhs[0], cmd_str, cmd_str_len);

	//printf("cmd_str: %s\n", cmd_str);
	//int k;
	//scanf("Press any key to continue: %d", &k);

	auto cmd_iter = cmd_list.find(std::string(cmd_str));
	if(cmd_iter == cmd_list.end()){
		mexErrMsgTxt(cv::format("Invalid comand provided: %s.", cmd_str).c_str());
	}
	const int cmd_id = cmd_iter->second;
	switch(cmd_id) {
	case MEX_TRACKER_CREATE:
	{
		if(nrhs > 1){
			if(!mxIsChar(prhs[1])){
				mexErrMsgTxt("Second input argument for creating tracker must be a string.");
			}
			int cfg_str_len = mxGetM(prhs[1])*mxGetN(prhs[1]) + 1;
			config_root_dir = (char *)mxMalloc(cfg_str_len);
			mxGetString(prhs[1], config_root_dir, cfg_str_len);
		}
		if(!createTracker()){
			*ret_val = 0;
			return;
		}
		tracker_created = true;
		if(trackers.size() == 0){
			tracker_id = 0;
		} else{
			tracker_id = trackers.size() - 1;
		}
		*ret_val = 1;
		return;
	}
	case MEX_TRACKER_INITIALIZE:
	{
		if(nlhs != 2){
			mexErrMsgTxt("2 output arguments are needed to initialize tracker.");
		}
		if(!tracker_created){
			mexErrMsgTxt("Tracker must be created before it can be initialized.");
		}
		if(nrhs > 3){
			tracker_id = getTrackerID(prhs[3]);
		}
		cv::Mat init_img = getImage(prhs[1]);
		cv::Mat init_corners;
		if(nrhs > 2){
			init_corners = getCorners(prhs[2]);
		} else{
			mtf::utils::ObjUtils obj_utils;
			try{
				if(!obj_utils.selectObjects(init_img, 1, patch_size, line_thickness,
					write_objs, sel_quad_obj, write_obj_fname.c_str())){
					mexErrMsgTxt("Object(s) to be tracked could not be obtained.\n");
				}
			} catch(const mtf::utils::Exception &err){
				mexErrMsgTxt(cv::format("Exception of type %s encountered while obtaining the objects to track: %s\n",
					err.type(), err.what()).c_str());
			}
			init_corners = obj_utils.getObj().corners.clone();
		}
		if(!initializeTracker(init_img, init_corners)){
			*ret_val = 0;
			return;
		}
		plhs[1] = setCorners(trackers[tracker_id]->getRegion());
		tracker_initialized = true;
		*ret_val = 1;
		return;
	}
	case MEX_TRACKER_UPDATE:
	{
		if(nrhs < 2){
			mexErrMsgTxt("At least 2 input arguments are needed to update tracker.");
		}
		if(nlhs != 2){
			mexErrMsgTxt("2 output arguments are needed to update tracker.");
		}
		if(!tracker_initialized){
			mexErrMsgTxt("Tracker must be initialized before it can be updated.");
		}
		if(nrhs > 2){
			tracker_id = getTrackerID(prhs[2]);
		}
		if(tracker_id >= trackers.size()){
			mexErrMsgTxt(cv::format("Tracker ID %d is invalid since only %d trackers have been created",
				tracker_id, trackers.size()).c_str());
		}
		cv::Mat curr_img = getImage(prhs[1]);
		if(!updateTracker(curr_img)){
			*ret_val = 0;
			return;
		}
		plhs[1] = setCorners(trackers[tracker_id]->getRegion());
		*ret_val = 1;
		return;
	}
	case MEX_TRACKER_SET_REGION:
	{
		if(nrhs < 2){
			mexErrMsgTxt("At least 2 input arguments are needed to reset tracker.");
		}
		if(nlhs != 2){
			mexErrMsgTxt("2 output arguments are needed to reset tracker.");
		}
		if(!tracker_initialized){
			mexErrMsgTxt("Tracker must be initialized before it can be reset.");
		}
		if(nrhs > 2){
			tracker_id = getTrackerID(prhs[2]);
		}
		if(tracker_id >= trackers.size()){
			mexErrMsgTxt(cv::format("Tracker ID %d is invalid since only %d trackers have been created",
				tracker_id, trackers.size()).c_str());
		}
		cv::Mat curr_corners = getCorners(prhs[1]);
		if(!setRegion(curr_corners)){
			*ret_val = 0;
			return;
		}
		plhs[1] = setCorners(trackers[tracker_id]->getRegion());
		*ret_val = 1;
		return;
	}
	case MEX_TRACKER_REMOVE:
	{
		if(nlhs != 1){
			mexErrMsgTxt("1 output argument is needed to reset tracker.");
		}
		if(nrhs > 1){
			tracker_id = getTrackerID(prhs[1]);
		}
		if(tracker_id >= trackers.size()){
			mexErrMsgTxt(cv::format("Tracker ID %d is invalid since only %d trackers have been created",
				tracker_id, trackers.size()).c_str());
		}
		trackers.erase(trackers.begin() + tracker_id);
		if(trackers.size() == 0){
			tracker_id = 0;
		} else{
			tracker_id = trackers.size() - 1;
		}
		*ret_val = 1;
		return;
	}
	default:
		mexErrMsgTxt(cv::format("Invalid comand provided: %s.", cmd_str).c_str());
	}
}
