#include "algo.hpp"
#include "draw.hpp"
#include "util.hpp"
#include "blend.hpp"
#include "settings.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>

#include <opencv2/core/core.hpp>

using namespace std;
using namespace cv;

namespace poppy {

void make_delaunay_mesh(const Size &size, Subdiv2D &subdiv, vector<Point2f> &dstPoints) {
	vector<Vec6f> triangleList;
	subdiv.getTriangleList(triangleList);
	vector<Point2f> pt(3);
	Rect rect(0, 0, size.width, size.height);

	for (size_t i = 0; i < triangleList.size(); i++) {
		Vec6f t = triangleList[i];
		pt[0] = Point2f(cvRound(t[0]), cvRound(t[1]));
		pt[1] = Point2f(cvRound(t[2]), cvRound(t[3]));
		pt[2] = Point2f(cvRound(t[4]), cvRound(t[5]));

		if (rect.contains(pt[0]) && rect.contains(pt[1]) && rect.contains(pt[2])) {
			dstPoints.push_back(pt[0]);
			dstPoints.push_back(pt[1]);
			dstPoints.push_back(pt[2]);
		}
	}
}

Mat points_to_homogenous_mat(const vector<Point> &pts) {
	int numPts = pts.size();
	Mat homMat(3, numPts, CV_32FC1);
	for (int i = 0; i < numPts; i++) {
		homMat.at<float>(0, i) = pts[i].x;
		homMat.at<float>(1, i) = pts[i].y;
		homMat.at<float>(2, i) = 1.0;
	}
	return homMat;
}

void morph_points(vector<Point2f> &srcPts1, vector<Point2f> &srcPts2, vector<Point2f> &dstPts, float s) {
	assert(srcPts1.size() == srcPts2.size());
	int numPts = srcPts1.size();
	dstPts.resize(numPts);
	for (int i = 0; i < numPts; i++) {
		dstPts[i].x = ((1.0 - s) * srcPts1[i].x + s * srcPts2[i].x);
		dstPts[i].y = ((1.0 - s) * srcPts1[i].y + s * srcPts2[i].y);
	}
}

void get_triangle_indices(const Subdiv2D &subDiv, const vector<Point2f> &points, vector<Vec3i> &triangleVertices) {
	vector<Vec6f> triangles;
	subDiv.getTriangleList(triangles);

	int numTriangles = triangles.size();
	triangleVertices.clear();
	triangleVertices.reserve(numTriangles);
	for (int i = 0; i < numTriangles; i++) {
		vector<Point2f>::const_iterator vert1, vert2, vert3;
		vert1 = find(points.begin(), points.end(), Point2f(triangles[i][0], triangles[i][1]));
		vert2 = find(points.begin(), points.end(), Point2f(triangles[i][2], triangles[i][3]));
		vert3 = find(points.begin(), points.end(), Point2f(triangles[i][4], triangles[i][5]));

		Vec3i vertex;
		if (vert1 != points.end() && vert2 != points.end() && vert3 != points.end()) {
			vertex[0] = vert1 - points.begin();
			vertex[1] = vert2 - points.begin();
			vertex[2] = vert3 - points.begin();
			triangleVertices.push_back(vertex);
		}
	}
}

void make_triangler_points(const vector<Vec3i> &triangleVertices, const vector<Point2f> &points, vector<vector<Point>> &trianglerPts) {
	int numTriangles = triangleVertices.size();
	trianglerPts.resize(numTriangles);
	for (int i = 0; i < numTriangles; i++) {
		vector<Point> triangle;
		for (int j = 0; j < 3; j++) {
			triangle.push_back(Point(points[triangleVertices[i][j]].x, points[triangleVertices[i][j]].y));
		}
		trianglerPts[i] = triangle;
	}
}

void paint_triangles(Mat &img, const vector<vector<Point>> &triangles) {
	int numTriangles = triangles.size();

	for (int i = 0; i < numTriangles; i++) {
		vector<Point> poly(3);

		for (int j = 0; j < 3; j++) {
			poly[j] = Point(cvRound(triangles[i][j].x), cvRound(triangles[i][j].y));
		}
		fillConvexPoly(img, poly, Scalar(i + 1));
	}
}

void solve_homography(const vector<Point> &srcPts1, const vector<Point> &srcPts2, Mat &homography) {
	assert(srcPts1.size() == srcPts2.size());
	homography = points_to_homogenous_mat(srcPts2) * points_to_homogenous_mat(srcPts1).inv();
}

void solve_homography(const vector<vector<Point>> &srcPts1,
		const vector<vector<Point>> &srcPts2,
		vector<Mat> &hmats) {
	assert(srcPts1.size() == srcPts2.size());

	int ptsNum = srcPts1.size();
	hmats.clear();
	hmats.reserve(ptsNum);
	for (int i = 0; i < ptsNum; i++) {
		Mat homography;
		solve_homography(srcPts1[i], srcPts2[i], homography);
		hmats.push_back(homography);
	}
}

void morph_homography(const Mat &Hom, Mat &MorphHom1, Mat &MorphHom2, float blend_ratio) {
	Mat invHom = Hom.inv();
	MorphHom1 = Mat::eye(3, 3, CV_32FC1) * (1.0 - blend_ratio) + Hom * blend_ratio;
	MorphHom2 = Mat::eye(3, 3, CV_32FC1) * blend_ratio + invHom * (1.0 - blend_ratio);
}

void morph_homography(const vector<Mat> &homs,
		vector<Mat> &morphHoms1,
		vector<Mat> &morphHoms2,
		float blend_ratio) {
	int numHoms = homs.size();
	morphHoms1.resize(numHoms);
	morphHoms2.resize(numHoms);
	for (int i = 0; i < numHoms; i++) {
		morph_homography(homs[i], morphHoms1[i], morphHoms2[i], blend_ratio);
	}
}

void create_map(const Mat &triangleMap, const vector<Mat> &homMatrices, Mat &mapx, Mat &mapy) {
	assert(triangleMap.type() == CV_32SC1);

	// Allocate Mat for the map
	mapx.create(triangleMap.size(), CV_32FC1);
	mapy.create(triangleMap.size(), CV_32FC1);

	// Compute inverse matrices
	vector<Mat> invHomMatrices(homMatrices.size());
	for (size_t i = 0; i < homMatrices.size(); i++) {
		invHomMatrices[i] = homMatrices[i].inv();
	}

	for (int y = 0; y < triangleMap.rows; y++) {
		for (int x = 0; x < triangleMap.cols; x++) {
			int idx = triangleMap.at<int>(y, x) - 1;
			if (idx >= 0) {
				Mat H = invHomMatrices[triangleMap.at<int>(y, x) - 1];
				float z = H.at<float>(2, 0) * x + H.at<float>(2, 1) * y + H.at<float>(2, 2);
				if (z == 0)
					z = 0.00001;
				mapx.at<float>(y, x) = (H.at<float>(0, 0) * x + H.at<float>(0, 1) * y + H.at<float>(0, 2)) / z;
				mapy.at<float>(y, x) = (H.at<float>(1, 0) * x + H.at<float>(1, 1) * y + H.at<float>(1, 2)) / z;
			}
			else {
				mapx.at<float>(y, x) = x;
				mapy.at<float>(y, x) = y;
			}
		}
	}
}

double morph_images(const Mat& img1, const Mat& img2, const Mat &corrected1, const Mat &corrected2, const Mat& gabor2, Mat &goodFeatures1, Mat &goodFeatures2, Mat &dst, const Mat &last, vector<Point2f> &morphedPoints, vector<Point2f> srcPoints1, vector<Point2f> srcPoints2, double shapeRatio, double maskRatio, double linear) {
	Size SourceImgSize(img1.cols, img1.rows);
	Subdiv2D subDiv1(Rect(0, 0, SourceImgSize.width, SourceImgSize.height));
	Subdiv2D subDiv2(Rect(0, 0, SourceImgSize.width, SourceImgSize.height));
	Subdiv2D subDivMorph(Rect(0, 0, SourceImgSize.width, SourceImgSize.height));

	vector<Point2f> uniq1, uniq2, uniqMorph;
	clip_points(srcPoints1, img1.cols, img1.rows);
	check_points(srcPoints1, img1.cols, img1.rows);
	make_uniq(srcPoints1, uniq1);
	check_uniq(uniq1);
	subDiv1.insert(uniq1);

	clip_points(srcPoints2, img1.cols, img1.rows);
	check_points(srcPoints2, img1.cols, img1.rows);
	make_uniq(srcPoints2, uniq2);
	check_uniq(uniq2);
	subDiv2.insert(uniq2);

	if (uniq1.size() > uniq2.size())
		uniq1.resize(uniq2.size());
	else
		uniq2.resize(uniq1.size());

	morph_points(srcPoints1, srcPoints2, morphedPoints, shapeRatio);
	assert(srcPoints1.size() == srcPoints2.size() && srcPoints2.size() == morphedPoints.size());

	clip_points(morphedPoints, img1.cols, img1.rows);
	check_points(morphedPoints, img1.cols, img1.rows);
	make_uniq(morphedPoints, uniqMorph);
	check_uniq(uniqMorph);
	subDivMorph.insert(uniqMorph);

	// Get the ID list of corners of Delaunay triangles.
	vector<Vec3i> triangleIndices;
	get_triangle_indices(subDivMorph, morphedPoints, triangleIndices);

	// Get coordinates of Delaunay corners from ID list
	vector<vector<Point>> triangleSrc1, triangleSrc2, triangleMorph;
	make_triangler_points(triangleIndices, srcPoints1, triangleSrc1);
	make_triangler_points(triangleIndices, srcPoints2, triangleSrc2);
	make_triangler_points(triangleIndices, morphedPoints, triangleMorph);

	// Create a map of triangle ID in the morphed image.
	Mat triMap = Mat::zeros(SourceImgSize, CV_32SC1);
	paint_triangles(triMap, triangleMorph);

	// Compute Homography matrix of each triangle.
	vector<Mat> homographyMats, morphHom1, morphHom2;
	solve_homography(triangleSrc1, triangleSrc2, homographyMats);
	morph_homography(homographyMats, morphHom1, morphHom2, shapeRatio);

	Mat trImg1;
	Mat trans_map_x1, trans_map_y1;
	create_map(triMap, morphHom1, trans_map_x1, trans_map_y1);
	remap(corrected1, trImg1, trans_map_x1, trans_map_y1, INTER_LINEAR);

	Mat trImg2;
	Mat trans_map_x2, trans_map_y2;
	create_map(triMap, morphHom2, trans_map_x2, trans_map_y2);
	remap(corrected2, trImg2, trans_map_x2, trans_map_y2, INTER_LINEAR);

	homographyMats.clear();
	morphHom1.clear();
	morphHom2.clear();
	triMap.release();

	Mat_<Vec3f> l;
	Mat_<Vec3f> r;
	trImg1.convertTo(l, CV_32F, 1.0 / 255.0);
	trImg2.convertTo(r, CV_32F, 1.0 / 255.0);

	Mat m2;
	cvtColor(gabor2, m2, COLOR_BGR2GRAY);
	m2 = 1.0 - m2;
//	show_image("m2", m2);

	Mat ones = Mat::ones(m2.size(), m2.type());
	Mat lbmask = (ones * (1.0 - maskRatio)) - (m2 * maskRatio);
	lbmask.setTo(0.0, lbmask < 0);
	lbmask.setTo(1.0, lbmask > 1);
	show_image("lbmask2", lbmask);

	LaplacianBlending lb(l, r, lbmask, Settings::instance().pyramid_levels);
	Mat_<Vec3f> lapBlend = lb.blend();
	double amount = sin(maskRatio * M_PI);
	dst = unsharp_mask(lapBlend, 1, 1.0 - amount, 0.3);
	dst.convertTo(dst, CV_8U, 255);
	Mat analysis = dst.clone();
	Mat prev = last.clone();
	if (prev.empty())
		prev = dst.clone();
	draw_morph_analysis(dst, prev, analysis, SourceImgSize, subDiv1, subDiv2, subDivMorph, { 0, 0, 255 });
	show_image("mesh", analysis);
	return 0;
}
}
