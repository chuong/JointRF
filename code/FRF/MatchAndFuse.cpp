#include"stdafx.h"
#include<opencv2/opencv.hpp>
#include<opencv2/xfeatures2d.hpp>
#include<iostream>
#include"guidedFilter.h"
#include"MatchAndFuse.h"
#include<map>
#include<fstream>
#define _DEBUG
#define TOL_RANGE 10
#define minHessian 1000

int MF::tth = 0;
double MF::ave_time = 0;
int MF::kkth = 0;

MF::MF(cv::Mat _src_img) {
	_src_img.copyTo(src_img);
	saliency_map = cv::Mat::zeros(cv::Size(src_img.cols,src_img.rows), CV_32FC1);
	s_matched_map= cv::Mat::zeros(cv::Size(src_img.cols, src_img.rows), CV_32FC1);
	activity_map= cv::Mat::zeros(cv::Size(src_img.cols, src_img.rows), CV_32FC1);
	keypoints= std::vector<cv::KeyPoint>();
	
}

void MF::GetSaliencyMap(const cv::Mat& img, cv::Mat& saliency_map, std::vector<cv::KeyPoint>& keypoints, cv::Mat& keyimg, const int octave, const int layer) {

	keypoints.clear();
	cv::Ptr<cv::xfeatures2d::SurfFeatureDetector>detector = cv::xfeatures2d::SurfFeatureDetector::create(minHessian, octave, layer,false,true);
	std::vector<cv::KeyPoint> kpts;//The keypoints detected by initial code.
	std::vector<cv::DetPoint> dpts;//The maximum of the local window.
 	detector->detect(img, kpts, cv::noArray(), dpts);
	//-------------------Draw the keypoints------------------------------------------
	cv::Mat c_img = img.clone();
	cv::Mat d_img;
	cv::drawKeypoints(c_img, kpts, d_img, cv::Scalar::all(-1),cv::DrawMatchesFlags::DEFAULT);
	keyimg = d_img;
	//-------------------------------------------------------------------------------

	keypoints = kpts;

	//---------For SURF----------
	std::sort(dpts.begin(), dpts.end(), [](cv::DetPoint& a, cv::DetPoint& b) {return a.octave_num < b.octave_num;});
	//----------------------------
	int real_o_num= dpts[dpts.size() - 1].octave_num + 1;
	int sel_o_num = dpts[dpts.size() - 1].octave_num -4;//Too much octaves are not good enough.
	int max_o_num = 0;
	
	sel_o_num > 0 ? max_o_num = sel_o_num : max_o_num = real_o_num;

	std::vector<cv::Mat>octave_maps(max_o_num);//Save the maps for the max response.


	int max_cols = img.cols;
	int max_rows = img.rows;
	for (size_t odx = 0;odx < max_o_num;odx++) {//Create the maps for each octave to prepare for the max_maps.
		int ratio = std::pow(2., odx);
		octave_maps[odx] = cv::Mat::zeros(cv::Size(max_cols/ratio, max_rows/ratio), CV_32FC1);
	}

	for (size_t pdx = 0;pdx < dpts.size();pdx++) {//Generate the max_map with the detected points.
		if (dpts[pdx].octave_num<max_o_num) {//Igonore the enlarged layers.
			int o_num = dpts[pdx].octave_num;
			float val = dpts[pdx].value;
			int d_x = dpts[pdx].dp.x;
			int d_y = dpts[pdx].dp.y;
			cv::Mat& o_map = octave_maps[o_num];
			if (val>o_map.at<float>(d_y, d_x)) {//Find the maximum in each position for corresponding octave.
				o_map.at<float>(d_y, d_x) = val;
			}
		}
	}

	dpts.clear();

	//Resize the images into the same size.
    for (size_t mdx = 0;mdx < max_o_num;mdx++) {//Only resize the actual octaves.
		cv::Mat size_map;
		cv::Mat& temp_img = octave_maps[mdx];
		resize(temp_img, size_map, cv::Size(img.cols, img.rows), 0, 0);
		temp_img = size_map;
	}
	//-------------------------------------------------
	//Construct the saliency map by choosing the max value from each octave_map.
	cv::Mat max_map = cv::Mat::zeros(cv::Size(img.cols, img.rows), CV_32FC1);

	for (size_t sdx = 0; sdx < max_o_num; sdx++) {
		max_map = cv::max(max_map, octave_maps[sdx]);
	}
	//---------------------------------
	saliency_map = max_map;
}

void MF::MatchMap(const MF& s_img, const MF& m_img, cv::Mat& s_matched_img,cv::Mat& i_matched_img, cv::Mat& pair_match,cv::Mat& fine_pairmatch, double& ratio, int& val_num, const int octave, const int layer, const bool flag) {

	//-------------
	int det_num = s_img.keypoints.size();
	//-------------
	if (s_img.keypoints.size() == 0) {
		val_num = 0;
		ratio =0;
		return;
	}
	const cv::Mat f_img = s_img.src_img;
	const std::vector<cv::KeyPoint> f_keypoints = s_img.keypoints;
	const cv::Mat base_img = m_img.src_img;
	const std::vector<cv::KeyPoint> base_keypoints = m_img.keypoints;

	cv::Mat f_gray, s_gray;
	if (f_img.channels() == 3) {
		cv::cvtColor(f_img, f_gray, CV_RGB2GRAY);
		cv::cvtColor(base_img, s_gray, CV_RGB2GRAY);
	}
	else {
		f_img.copyTo(f_gray);
		base_img.copyTo(s_gray);
	}

	//Calculate the descriptors.
	cv::Ptr<cv::xfeatures2d::SurfDescriptorExtractor>f_extractor = cv::xfeatures2d::SurfFeatureDetector::create(minHessian,octave, layer, false, true);
	cv::Mat f_descriptor;
	std::vector<cv::KeyPoint>ff_keypoints(f_keypoints);
	f_extractor->compute(f_gray, ff_keypoints, f_descriptor);

	if (!flag) {//Calculate the descriptors of the base image only once.

		cv::Ptr<cv::xfeatures2d::SurfDescriptorExtractor>s_extractor = cv::xfeatures2d::SurfFeatureDetector::create(minHessian, octave, layer, false , true);
		cv::Mat s_descriptor;
		std::vector<cv::KeyPoint>ss_keypoints(base_keypoints);
		s_extractor->compute(s_gray, ss_keypoints, s_descriptor);
		MF::base_descriptor = s_descriptor;
	}

	//Match the images. All the images are matched with the base_image.
	cv::Ptr<cv::DescriptorMatcher>matcher = cv::DescriptorMatcher::create("BruteForce");
	std::vector<cv::DMatch>imatches;
	//------Test the speed for different descritptor dimensions.----------------

	matcher->match(f_descriptor, base_descriptor, imatches);
	std::sort(imatches.begin(), imatches.end(), [](cv::DMatch& a, cv::DMatch& b) {return a.distance < b.distance;});
	//To accelerate the algorithm, only reserve the first 20 matched points.
	std::vector<cv::DMatch>matches;
	
	int d_matchs = imatches.size();
	imatches.size() > 20 ? matches.assign(imatches.begin(), imatches.begin() + 20): (matches = imatches);
	
#ifdef _DEBUG
	//Display the matched points
	cv::Mat outmatches;
	cv::drawMatches(f_img, f_keypoints, base_img, base_keypoints, matches, outmatches, cv::Scalar(0, 0, 255), cv::Scalar(0, 0, 255));
	pair_match = outmatches;
#endif
	//Transfer the matches into points.
	std::vector<cv::Point2f> f_points;
	std::vector<cv::Point2f> base_points;
	for (size_t mdx = 0;mdx < matches.size();mdx++) {
		cv::Point2f fp = f_keypoints[matches[mdx].queryIdx].pt;
		cv::Point2f basep = base_keypoints[matches[mdx].trainIdx].pt;
		f_points.push_back(fp);
		base_points.push_back(basep);
	}
	//Store the reasonable pairs of points by choosing the possible shift distance.
	//Define a structure to record the data of the votes and corresponding distance.
	typedef struct {
		int vote;
		int x_dis;
		int y_dis;
		std::vector<int>x_difs;
		std::vector<int>y_difs;
	}vd;

	vd temp;
	temp.x_dis = 0;
	temp.y_dis = 0;
	temp.vote = 0;
	std::vector<vd>vds;
	vds.push_back(temp);

	size_t sth = 0;

	std::vector<cv::DMatch> n_matches;
	for (size_t fth = 0;fth < f_points.size();fth++) {
		int dy = f_points[fth].y - base_points[fth].y;
		int dx = f_points[fth].x - base_points[fth].x;

		//--------------------------------------
		n_matches.push_back(matches[fth]);
		for (sth = 0; sth < vds.size(); sth++) {
			int x_simi = abs(dx - vds[sth].x_dis);
			int y_simi = abs(dy - vds[sth].y_dis);
			if (x_simi<TOL_RANGE && y_simi<TOL_RANGE) {
				if (dx < vds[sth].x_dis) {
					vds[sth].x_dis = dx;
				}
				if (dy < vds[sth].y_dis) {
					vds[sth].y_dis = dy;
				}
				vds[sth].vote++;
				vds[sth].x_difs.push_back(dx);
				vds[sth].y_difs.push_back(dy);
				break;
			} 
		}

		if (sth==vds.size()) {
				vd temp;
				temp.x_dis = dx;
				temp.y_dis = dy;
				temp.vote = 1;
				temp.x_difs.push_back(dx);
				temp.y_difs.push_back(dy);
				vds.push_back(temp);
		}

	}//End for all the matched points.

#ifdef _DEBUG
	 //Display the matched points
	cv::Mat n_outmatches;
	cv::drawMatches(f_img, f_keypoints, base_img, base_keypoints, n_matches, n_outmatches, cv::Scalar(0, 0, 255), cv::Scalar(0, 0, 255));
	fine_pairmatch = n_outmatches;
#endif

	if (n_matches.size()==0) {//Save the original image as the matched image.
		s_matched_img = s_img.saliency_map;
		i_matched_img = s_img.src_img;
		return;
	}
	else {//Select the highest vote to construct the transfer matrix.

		std::sort(vds.begin(), vds.end(), [](vd& a, vd& b) {return a.vote < b.vote;});
		int max_vote = vds[vds.size() - 1].vote;

		val_num = max_vote;
		int match_size = f_points.size();
		ratio = (double)val_num / match_size;

		std::sort(vds[vds.size() - 1].x_difs.begin(), vds[vds.size() - 1].x_difs.end());
		std::sort(vds[vds.size() - 1].y_difs.begin(), vds[vds.size() - 1].y_difs.end());
		int d_length = vds[vds.size() - 1].x_difs.size()/2;
		int x_min_dis = vds[vds.size() - 1].x_difs[d_length];
		int y_min_dis = vds[vds.size() - 1].y_difs[d_length];

		vds.clear();

		//Construct the transform matrix
		int x_t = x_min_dis;
		int y_t = y_min_dis;
		cv::Mat homo = (cv::Mat_<double>(2,3)<<1,0,-x_t,0,1,-y_t);
		//Warp the saliency map.
		cv::Mat saliency_map = s_img.saliency_map;
		cv::Mat i_img = s_img.src_img;
		cv::Mat s_warp_img = cv::Mat::zeros(cv::Size(saliency_map.cols, saliency_map.rows), saliency_map.type());
		cv::Mat i_warp_img = cv::Mat::zeros(cv::Size(i_img.cols,i_img.rows),i_img.type());
		cv::warpAffine(saliency_map, s_warp_img, homo, cv::Size(saliency_map.cols, saliency_map.rows));
		s_matched_img = s_warp_img;
		cv::warpAffine(i_img,i_warp_img,homo,cv::Size(i_img.cols,i_img.rows));

		//Crop the i_warp_img.
		cv::Point2f tl,br;
		cv::Mat i_w_gray;
		cv::cvtColor(i_warp_img,i_w_gray,CV_RGB2GRAY);
		bool flag = 0;
		for (size_t ith = 0;ith < i_w_gray.rows;ith++) {//Find the tl point.
			for (size_t jth = 0;jth < i_w_gray.cols;jth++) {
				if (i_w_gray.at<uchar>(ith,jth)!=0) {
					tl.x = jth;
					tl.y = ith;
					flag = 1;
					break;
				}
			}
			if (flag==1) {
				flag = 0;
				break;
			}
		}

		for (size_t bth = i_w_gray.rows - 1;bth > 0;bth--) {//Find the br point.
			for (size_t rth = i_w_gray.cols - 1;rth > 0;rth--) {
				if (i_w_gray.at<uchar>(bth,rth)!=0) {
					br.x = rth;
					br.y = bth;
					flag = 1;
					break;
				}
			}
			if (flag==1) {
				flag = 0;
				break;
			}
		}
		cv::Mat temp_base_img = base_img.clone();
		i_warp_img(cv::Rect(tl, br)).copyTo(temp_base_img(cv::Rect(tl,br)));
		i_matched_img = temp_base_img;
	}
}

void MF::GetActivityMap(const std::vector<MF>& s_imgs, cv::Mat& activity_map,const int s,const bool button,const int r,const double eps) {
	//Get the origianl mask of the image.
	if(!button){
		cv::Mat max_matched_map = cv::Mat::zeros(cv::Size(s_imgs[0].s_matched_map.cols, s_imgs[0].s_matched_map.rows), s_imgs[0].s_matched_map.type());
		for (size_t sdx = 0;sdx < s_imgs.size();sdx++) {
			max_matched_map = cv::max(max_matched_map, s_imgs[sdx].s_matched_map);
		}
		std::vector<cv::Mat> o_masks;
		for (size_t odx = 0;odx < s_imgs.size();odx++) {
			cv::Mat o_mask;
			o_mask = (s_imgs[odx].s_matched_map == max_matched_map);
			o_masks.push_back(o_mask);
		}

		//To insure the pixel value of each position only comes from one image.
		cv::Mat sum_mask = cv::Mat::zeros(cv::Size(o_masks[0].cols, o_masks[0].rows), o_masks[0].type());
		for (size_t mdx = 0;mdx < o_masks.size();mdx++) {
			cv::Mat temp_mask = o_masks[mdx].mul(1. / 255);
			sum_mask = sum_mask + temp_mask;
		}

		std::vector<cv::Point2f>over_points;
		for (size_t k = 0;k < sum_mask.rows;k++) {//Record the location of over value.
			for (size_t m = 0;m < sum_mask.cols;m++) {
				if (int(sum_mask.at<uchar>(k, m))>1) {
					cv::Point2f f;
					f.x = m;
					f.y = k;
					over_points.push_back(f);
				}
			}
		}

		for (size_t t = 0;t < over_points.size();t++) {//For each over value point.
			cv::Point2f p = over_points[t];
			for (size_t w = 0;w < o_masks.size();w++) {//For each original mask.
				if (int(o_masks[w].at<uchar>(p.y, p.x)) == 255) {
					for (size_t r = w + 1;r < o_masks.size();r++) {//Set the value of the rest original masks as 0 in the corresponding position.
						o_masks[r].at<uchar>(p.y, p.x) = 0;
					}
					break;
				}
			}
		}//End for all the over value points.
		MF::i_masks = o_masks;
		
	}
	//Refine the original masks with guided filter.
	cv::Mat fine_mask;
	cv::Mat src_gray;
	if (s_imgs[s].src_img.channels()==3) {
		cv::cvtColor(s_imgs[s].src_img,src_gray,CV_RGB2GRAY);
	}
	else {
		src_gray= s_imgs[s].src_img;
	}
	fine_mask=guidedFilter(src_gray,i_masks[s],r,eps);
	activity_map = fine_mask;

}