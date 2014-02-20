#include "reconstruct.h"

ChainFeatureMatcher::ChainFeatureMatcher() :
    numPoints(0) {
    prevPtGlobalPt = unique_ptr<map<int, int>>(new map<int, int>());
    curPtGlobalPt = unique_ptr<map<int, int>>(new map<int, int>());
}

void ChainFeatureMatcher::processNext(
        const CImg<uint8_t>& gray) {
    if (!curMatcher) {
        curMatcher = unique_ptr<CVFeatureMatcher>(new CVFeatureMatcher(maxFeatureCount));
    }

    int numFeatures = curMatcher->detectFeatures(gray);

    printf("numFeatures = %d\n", numFeatures);

    curPtGlobalPt->clear();

    vector<tuple<int, int>> matchBuf;

    if (prevMatcher) {
        prevMatcher->match(*curMatcher, matchBuf, maxMatchCount);

        if (matches.size() == 0) {
            matches.push_back(vector<tuple<int, float, float>>());
        }

        matches.push_back(vector<tuple<int, float, float>>());

        vector<tuple<int, float, float>>& prevMatches = matches[matches.size() - 2];
        vector<tuple<int, float, float>>& curMatches = matches[matches.size() - 1];

        for (const tuple<int, int>& match : matchBuf) {
            int prevPtIndex = get<0>(match);
            int curPtIndex = get<1>(match);

            int globalPtIndex;
            
            bool isNewPoint = (prevPtGlobalPt->count(prevPtIndex) == 0);

            if (isNewPoint) {
                globalPtIndex = numPoints;

                numPoints++;
            } else {
                globalPtIndex = (*prevPtGlobalPt)[prevPtIndex];
            }

            (*prevPtGlobalPt)[prevPtIndex] = globalPtIndex;
            (*curPtGlobalPt)[curPtIndex] = globalPtIndex;

            float x, y;

            if (isNewPoint) {
                prevMatcher->getKeypoint(prevPtIndex, x, y);

                // Transform the keypoint to image space
                x *= 2.0f / prevWidth;
                y *= 2.0f / prevHeight;

                x -= 1.0f;
                y -= 1.0f;
                
                prevMatches.push_back(make_tuple(globalPtIndex, x, y));
            }

            curMatcher->getKeypoint(curPtIndex, x, y);
            
            // Transform the keypoint to image space
            x *= 2.0f / gray.width();
            y *= 2.0f / gray.height();

            x -= 1.0f;
            y -= 1.0f;

            curMatches.push_back(make_tuple(globalPtIndex, x, y));
        }
    }

    swap(curMatcher, prevMatcher);
    swap(curPtGlobalPt, prevPtGlobalPt);

    prevWidth = gray.width();
    prevHeight = gray.height();
}

void ChainFeatureMatcher::visualizeFeatureMatches(
        function<const CImg<uint8_t>&(int)> imgLoader) const {
    const CImg<uint8_t>* prevImg = NULL;
    const CImg<uint8_t>* curImg = NULL;

    prevImg = &imgLoader(0);

    for (int i = 0; i < matches.size() - 1; i++) {
        curImg = &imgLoader(i + 1);
        
        assert(prevImg != NULL);
        assert(curImg != NULL);

        CImg<uint8_t> annotation(
                prevImg->width() + curImg->width(),
                max(prevImg->height(), curImg->height()),
                1, 1);

        annotation.draw_image(0, 0, 0, 0, *prevImg);

        annotation.draw_image(prevImg->width(), 0, 0, 0, *curImg);

        annotation.resize(-100, -100, -100, 3);

        CImg<uint8_t> col = CImg<uint8_t>::lines_LUT256();

        map<int, tuple<float, float>> prevMatchMap;

        for (const auto& match : matches[i]) {
            prevMatchMap[get<0>(match)] = make_tuple(get<1>(match), get<2>(match));
        }

        int matchI = 0;
        for (const auto& match : matches[i + 1]) {
            int pt = get<0>(match);

            float curX = get<1>(match);
            float curY = get<2>(match);

            curX = (curX + 1.0f) / 2.0f * curImg->width();
            curY = (curY + 1.0f) / 2.0f * curImg->height();

            if (prevMatchMap.count(pt) > 0) {
                uint8_t color[3];

                for (int c = 0; c < 3; c++) {
                    color[c] = col(matchI % 256, c);
                }

                const auto& prevXY = prevMatchMap[pt];

                float prevX = get<0>(prevXY);
                float prevY = get<1>(prevXY);

                prevX = (prevX + 1.0f) / 2.0f * prevImg->width();
                prevY = (prevY + 1.0f) / 2.0f * prevImg->height();

                annotation.draw_line(
                        (int) (prevX + 0.5f),
                        (int) (prevY + 0.5f),
                        (int) (curX + 0.5f) + prevImg->width(),
                        (int) (curY + 0.5f),
                        color);

                matchI++;
            }
        }

        annotation.display();
        
        swap(prevImg, curImg);
    }
}

ChainReconstruction::ChainReconstruction(
        const ChainFeatureMatcher* _features) :
        features(_features) {
}

void ChainReconstruction::solve() {
    const auto& matches = features->getObservations();

    int numPoints = features->getNumPoints();

    cameras.resize(matches.size());

    // Set initial camera values
    int camI = 0;
    for (CameraParam& cam : cameras) {
        cam[0] = 1;
        cam[1] = 0;
        cam[2] = 0;
        cam[3] = 0;
        cam[4] = (((double) rand()) / RAND_MAX) + (camI * 3);
        cam[5] = ((double) rand()) / RAND_MAX;
        cam[6] = ((double) rand()) / RAND_MAX;
        cam[7] = 1.0;
        cam[8] = 0.0;
        cam[9] = 0.0;

        camI++;
    }

    points.resize(numPoints);

    for (Point3d& p : points) {
        p[0] = (double) rand() / RAND_MAX;
        p[1] = (double) rand() / RAND_MAX;
        p[2] = (double) rand() / RAND_MAX - 10;
    }

    ceres::Problem problem;

    ceres::LossFunction* lossFunc = NULL; // new ceres::HuberLoss(25.0);

    for (int camI = 0; camI < matches.size(); camI++) {
        const vector<tuple<int, float, float>>& camMatches = matches[camI];

        for (const tuple<int, float, float>& match : camMatches) {
            ceres::CostFunction* costFunction = 
                new ceres::AutoDiffCostFunction<
                ceres::examples::SnavelyReprojectionErrorWithQuaternions, 2, 4, 6, 3>(
                        new ceres::examples::SnavelyReprojectionErrorWithQuaternions(
                            (double) get<1>(match),
                            (double) get<2>(match)));
            problem.AddResidualBlock(
                    costFunction,
                    lossFunc, // Squared loss
                    cameras[camI].data(),
                    cameras[camI].data() + 4,
                    points[get<0>(match)].data());
        }
    }

    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    options.max_num_iterations = 10000;
    options.minimizer_progress_to_stdout = true;

    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);
    cout << summary.FullReport() << endl;
}

void ChainReconstruction::exportPython(
    ostream& result) const {

    // Points

    result << "points = [\n";

    for (const Point3d& p : points) {
        result << "("
            << p[0] << ", " << p[1] << ", " << p[2]
            << "),\n";
    }

    result << "]\n";
    
    // Camera Translation

    result << "cameras_trans = [\n";

    for (const CameraParam& c : cameras) {
        result << "("
            << c[4] << ", " << c[5] << ", " << c[6]
            << "),\n";
    }

    result << "]\n";

    // Camera Rotation
    
    result << "cameras_rot = [\n";

    double mat[9];

    for (const CameraParam& c : cameras) {
        ceres::QuaternionToRotation(c.data(), mat);

        result << "(";

        for (int i = 0; i < 9; i++) {
            result << mat[i] << ", ";
        }

        result << "),\n";
    }

    result << "]\n";

    // Camera Intrinsics

    result << "cameras_intrin = [\n";

    for (const CameraParam& c : cameras) {
        result << "("
            << c[7] << ", " << c[8] << ", " << c[9]
            << "),\n";
    }

    result << "]\n";

    // Raw camera parameters

    result << "camera_raw = [\n";

    for (const CameraParam& c : cameras) {
        result << "(";

        for (auto p : c) {
            result << p << ", ";
        }

        result << "),\n";
    }

    result << "]\n";
}

