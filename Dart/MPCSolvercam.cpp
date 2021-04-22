#include "MPCSolvercam.hpp"

static std::ofstream foutDebug(realpath("../data/debug.txt", NULL), std::ofstream::trunc);

MPCSolvercam::MPCSolvercam(FootstepPlan* _footstepPlan, int sim, bool CAM, double torsomass, Eigen::Matrix3d MOI, Eigen::Vector3d theta_max) : footstepPlan(_footstepPlan) {

    this-> sim = sim;
    angleConstraint = CAM;
    ds_samples = doubleSupportSamples;


    if(angleConstraint){
        costFunctionHcam = Eigen::MatrixXd::Zero(4*N,2*N);
        costFunctionFcam = Eigen::VectorXd::Zero(4*N);
        AConstraintcam = Eigen::MatrixXd::Zero(4*N+2,2*N);
        bConstraintMincam = Eigen::VectorXd::Zero(4*N+2);
        bConstraintMaxcam = Eigen::VectorXd::Zero(4*N+2);
        costFunctionHcam = Eigen::MatrixXd::Zero(2*N,2*N);
        costFunctionFcam = Eigen::VectorXd::Zero(2*N);
        
        AAngleconstr = Eigen::MatrixXd::Zero(2*N,2*N);
        bAngleConstrMin = Eigen::VectorXd::Zero(2*N);
        bAngleConstrMax = Eigen::VectorXd::Zero(2*N);

        currentTheta = Eigen::Vector3d::Zero(3);
        currentThetaD = Eigen::Vector3d::Zero(3);
        currentThetaDD = Eigen::Vector3d::Zero(3);

        this->MOI = MOI;

        desiredTorque = Eigen::Vector3d::Zero(3);
        this->theta_max = theta_max;
    }
    else{
            AConstraintcam = Eigen::MatrixXd::Zero(2*N+2,2*N);
            bConstraintMincam = Eigen::VectorXd::Zero(2*N+2);
            bConstraintMaxcam = Eigen::VectorXd::Zero(2*N+2);
    }

    costFunctionHcam = Eigen::MatrixXd::Zero(2*N,2*N);
    costFunctionFcam = Eigen::VectorXd::Zero(2*N);
    AZmp_cam = Eigen::MatrixXd::Zero(2*N,2*N);
    bZmpMax_cam = Eigen::VectorXd::Zero(2*N);
    bZmpMin_cam = Eigen::VectorXd::Zero(2*N);
    Aeq_cam = Eigen::MatrixXd::Zero(2,N*2);
    beq_cam = Eigen::VectorXd::Zero(2);

    // Matrices for all constraints stacked        
    // Matrices for ZMP prediction
    p = Eigen::VectorXd::Ones(N);
    pmg = p*torsomass*9.81;

    // std::cout << pmg;

    P = Eigen::MatrixXd::Ones(N,N)*mpcTimeStep;
    Pmg = Eigen::MatrixXd::Ones(N,N)*controlTimeStep*torsomass*9.81;
    Eigen::MatrixXd Pthdd1 = Eigen::MatrixXd::Ones(N,N)*0.5*pow(controlTimeStep,2);
    Eigen::MatrixXd Pthdd2 = Eigen::MatrixXd::Ones(N,N)*controlTimeStep;

    for(int i = 0; i < N; ++i) {
    	for(int j = 0; j < N; ++j) {
            if (j > i) P(i, j)=0;
            if (j >= i) Pmg(i, j)=0;
            if (j > i+1) Pthdd1(i, j) = 0;
            if(j > i) Pthdd2(i, j) = 0;
        }
    }
    Pthdd = Pthdd1*P.transpose() + Pthdd2;

    double ch = cosh(omega*mpcTimeStep);
    double sh = sinh(omega*mpcTimeStep);

    Eigen::MatrixXd A_upd = Eigen::MatrixXd::Zero(3,3);
    Eigen::VectorXd B_upd = Eigen::VectorXd::Zero(3);
    A_upd<<ch,sh/omega,1-ch,omega*sh,ch,-omega*sh,0,0,1;
    B_upd<<mpcTimeStep-sh/omega,1-ch,mpcTimeStep;

    Eigen::RowVectorXd Vu_newline(N);
    Eigen::RowVectorXd Vs_newline(3);
    Eigen::RowVectorXd A_midLine(3);

    A_midLine<<omega*sh,ch,-omega*sh;

  // Midpoint of the ZMP constraints for anticipative tail
    x_midpoint.resize(500);
    y_midpoint.resize(500);
    int midpointFootstepIndex = 0;
    for (int i = 0; i < 500; i++) {
        int timeTillFootstepEnd = footstepPlan->getFootstepEndTiming(midpointFootstepIndex) - i;

        if (timeTillFootstepEnd <= 0) { // is footstep finished?
            midpointFootstepIndex++;
            timeTillFootstepEnd = footstepPlan->getFootstepEndTiming(midpointFootstepIndex) - i;
        }

        if (timeTillFootstepEnd > ds_samples) { // are we in single support?
            x_midpoint(i) = footstepPlan->getFootstepPosition(midpointFootstepIndex)(0);
            y_midpoint(i) = footstepPlan->getFootstepPosition(midpointFootstepIndex)(1);
        } else { // we are in double support
            x_midpoint(i) = footstepPlan->getFootstepPosition(midpointFootstepIndex)(0) * (double)timeTillFootstepEnd / ds_samples
                            + footstepPlan->getFootstepPosition(midpointFootstepIndex + 1)(0) * (1 - (double)timeTillFootstepEnd / ds_samples);
            y_midpoint(i) = footstepPlan->getFootstepPosition(midpointFootstepIndex)(1) * (double)timeTillFootstepEnd / ds_samples
                            + footstepPlan->getFootstepPosition(midpointFootstepIndex + 1)(1) * (1 - (double)timeTillFootstepEnd / ds_samples);
        }
    }
     old_fsCount = 0;
     adaptation_memo = 0;
     ct = 0;

        xz_dot_cam = 0.0;
        yz_dot_cam = 0.0;


}

MPCSolvercam::~MPCSolvercam() {}

State MPCSolvercam::solve(State current, State current_cam, WalkState walkState, double mass) {

    itr = walkState.mpcIter;
    fsCount = walkState.footstepCounter;

    // if (fsCount != old_fsCount) {
    //     adaptation_memo = 0;
    //     ds_samples = doubleSupportSamples;
    //     ct = 0;
    //     std::cout << "FOOTSTEP HAS CHANGEDcam" << '\n';
    //     std::cout << "footstepCountercam = "<< walkState.footstepCounter << '\n';
    // }

    std::vector<Eigen::VectorXd> fp = footstepPlan->getPlan();

    // Reset constraint matrices
    AZmp_cam.setZero();
    AAngleconstr.setZero();
    // Get the pose of the support foot in the world frame
    // Eigen::VectorXd supportFootPose = current.getSupportFootPose(walkState.supportFoot);

    // Construct some matrices that will be used later in the cost function and constraints
    // ************************************************************************************

    // Construct the Cc matrix, which maps iterations to predicted footsteps
    // Eigen::MatrixXd CcFull = Eigen::MatrixXd::Zero(N,M+1);

    // int fsAhead = 0;
    // for (int i = 0; i < N; ++i) {
    //     // if with the prediction index i we have reach the next footstep increase fsAhead
    //     if (footstepPlan->getFootstepStartTiming(walkState.footstepCounter) + walkState.mpcIter + i + 1 >= footstepPlan->getFootstepEndTiming(walkState.footstepCounter + fsAhead)) fsAhead++;

    //     // how many samples are left till the end of the current footstep?
    //     int samplesTillNextFootstep = footstepPlan->getFootstepEndTiming(walkState.footstepCounter + fsAhead) - (footstepPlan->getFootstepStartTiming(walkState.footstepCounter) + walkState.mpcIter + i + 1);

    //     // If it is the current footstep, it does not go in Cc, but subsequent ones do
    //     if (samplesTillNextFootstep > ds_samples) {
    //         CcFull(i, fsAhead) = 1;
    //     } else {
    //         CcFull(i, fsAhead) = (double)samplesTillNextFootstep / (double)ds_samples;
    //         CcFull(i, fsAhead + 1) = 1.0 - (double)samplesTillNextFootstep / (double)ds_samples;
    //     }
    // }

    // Eigen::VectorXd currentFootstepZmp = CcFull.col(0);
    // Eigen::MatrixXd Cc = CcFull.block(0,1,N,M);

    // // Construct the Ic matrix, which removes constraints from double support phases
    // Eigen::MatrixXd Ic = Eigen::MatrixXd::Identity(N,N);
    // for (int i = 0; i < N; ++i) {
    //     if (walkState.footstepCounter == 0 && walkState.mpcIter+i <= footstepPlan->getFootstepEndTiming(0)) Ic(i,i) = 0;
    // }

    // // Construct the difference matrix (x_j - x_{j-1})
    // Eigen::MatrixXd differenceMatrix = Eigen::MatrixXd::Identity(M,M);
    // for (int i = 0; i < M-1; ++i) {
    //     differenceMatrix(i+1,i) = -1;
    // }

    // // Retrieve footstep orientations over control horizon
    // Eigen::VectorXd predictedOrientations(M+1); // FIXME we would like to remove conditionals for the first step
    // for (int i = 0; i < M+1; i++) {
    //     if (walkState.footstepCounter - 2 + i >= 0) predictedOrientations(i) = footstepPlan->getFootstepOrientation(walkState.footstepCounter - 2 + i);
    //     else predictedOrientations(i) = footstepPlan->getFootstepOrientation(0);
    // }


    // Construct the stability constraint
    // **********************************
// // Periodic tail
    double stabConstrMultiplierP = (1-exp(-omega*mpcTimeStep)) / (1-pow(exp(-omega*mpcTimeStep),N));
    for(int i = 0; i < N; ++i) {
        Aeq_cam(0,i)     = stabConstrMultiplierP * exp(-omega*mpcTimeStep*i)/omega;
        Aeq_cam(1,N+i) = stabConstrMultiplierP * exp(-omega*mpcTimeStep*i)/omega;
    }
    
    beq_cam << current_cam.comPos(0) + current_cam.comVel(0)/omega - current_cam.zmpPos(0),
           current_cam.comPos(1) + current_cam.comVel(1)/omega - current_cam.zmpPos(1);

// //Truncated tail
    // double lambda_tail = exp(-omega*mpcTimeStep);
    // for(int i = 0; i < N; ++i) {
    //     Aeq_cam(0,i)  = (1/omega)*(1-lambda_tail)*exp(-omega*mpcTimeStep*i);
    //     Aeq_cam(1,N+i) = (1/omega)*(1-lambda_tail)*exp(-omega*mpcTimeStep*i);
    // }
    
    // beq_cam << current_cam.comPos(0) + current_cam.comVel(0)/omega - current_cam.zmpPos(0),
    //        current_cam.comPos(1) + current_cam.comVel(1)/omega - current_cam.zmpPos(1);


    // int prev = 200;

    // // add contribution to the tail: first contribution is the last footstep in the control horizon

    // Eigen::Vector3d anticipativeTail = Eigen::Vector3d::Zero();

    // // add contribution from the last footstep in the preview horizon (FIXME here we neglect the double support for simplicity)
    // // construct stability constraint with anticipative tail
    // for (int i = 0; i < prev; i++) {
    //     anticipativeTail(0) += exp(-omega*mpcTimeStep*(N + i)) * (1 - exp(-omega*mpcTimeStep)) * x_midpoint(walkState.mpcIter + N + i);
    //     anticipativeTail(1) += exp(-omega*mpcTimeStep*(N + i)) * (1 - exp(-omega*mpcTimeStep)) * y_midpoint(walkState.mpcIter + N + i);
    // }

    // anticipativeTail(0) += exp(-omega*mpcTimeStep*(N + prev)) * x_midpoint(walkState.mpcIter + N + prev);
    // anticipativeTail(1) += exp(-omega*mpcTimeStep*(N + prev)) * y_midpoint(walkState.mpcIter + N + prev);


    // double stabConstrMultiplier = (1-exp(-omega*mpcTimeStep)) / omega;

    // for(int i = 0; i < N; ++i) {
    //     Aeq_cam(0,i) = stabConstrMultiplier * exp(-omega*mpcTimeStep*i) - mpcTimeStep * exp(-omega*mpcTimeStep*N);
    //     Aeq_cam(1,N+i) = stabConstrMultiplier * exp(-omega*mpcTimeStep*i) - mpcTimeStep * exp(-omega*mpcTimeStep*N);
    // }

    // beq_cam << current_cam.comPos(0) + current_cam.comVel(0)/omega - current_cam.zmpPos(0) * (1.0 - exp(-omega*mpcTimeStep*N)) - anticipativeTail(0),
    //        current_cam.comPos(1) + current_cam.comVel(1)/omega - current_cam.zmpPos(1) * (1.0 - exp(-omega*mpcTimeStep*N)) - anticipativeTail(1);
     // Construct the ZMP constraint
    // ****************************


    // Construct the A matrix of the ZMP constraint, by diagonalizing two of the same, then rotating
    AZmp_cam.block(0,0,N,N) = P;
    AZmp_cam.block(N,N,N,N) = P;
    double xzcam_max = forceLimittorques/(mass*9.81);
    double yzcam_max = forceLimittorques/(mass*9.81);
    // xzcam_max = 10.0;
    // yzcam_max = 10.0;

        bZmpMin_cam << p*(-xzcam_max-current_cam.zmpPos(0)),p*(-yzcam_max-current_cam.zmpPos(1));
        bZmpMax_cam << p*(xzcam_max-current_cam.zmpPos(0)),p*(yzcam_max-current_cam.zmpPos(1));

        // std::cout << "xzcam_max = " << xzcam_max << std::endl;
        // std::cout << "yzcam_max = " << yzcam_max << std::endl;
        // std::cout << "-xzcam_max-current_cam.zmpPos(0) = " << -xzcam_max-current_cam.zmpPos(0) << std::endl;
        // std::cout << "xzcam_max-current_cam.zmpPos(0) = " << xzcam_max-current_cam.zmpPos(0) << std::endl;
    // Construct the Angle constraint
    // *******************************
        if(angleConstraint){
        desiredTorque << -current_cam.comPos(1)*mass*9.81, current_cam.comPos(0)*mass*9.81, 0.0;
        currentThetaDD = MOI.inverse()*desiredTorque;
        currentTheta += currentThetaD*mpcTimeStep + currentThetaDD*0.5*pow(mpcTimeStep,2);
        // std::cout << "currentTheta = " << currentTheta << std::endl;
        // std::cout << "torsoOrient = " << current_cam.torsoOrient << ", " << current.torsoOrient << std::endl;
        currentThetaD += currentThetaDD*mpcTimeStep;

        // actual current angular values

        // Eigen::Vector3d thetaMax << M_PI/6, M_PI/6, M_PI/6;
        Eigen::MatrixXd rhs = theta_max*(p.transpose())-currentTheta*(p.transpose()) - currentThetaD*(p.transpose()*P.transpose());
        rhs = MOI*rhs;
        rhs = rhs - (desiredTorque*p.transpose())*Pthdd;
        rhs = rhs.block(0,0,2,N);
        // rhs = rhs.transpose();
        Eigen::MatrixXd _rhs = rhs.transpose();
        

        Eigen::MatrixXd lhs = -theta_max*(p.transpose())-currentTheta*(p.transpose()) - currentThetaD*(p.transpose()*P.transpose());
        lhs = MOI*lhs;
        lhs = lhs - (desiredTorque*p.transpose())*Pthdd;
        lhs = lhs.block(0,0,2,N);
        // lhs = lhs.transpose();
        Eigen::MatrixXd _lhs = lhs.transpose();


    AAngleconstr.block(0,N,N,N) = -Pthdd.transpose()*Pmg;
    AAngleconstr.block(N,0,N,N) = Pthdd.transpose()*Pmg;

    // std::cout << "AAngleconstr.block(0,0,N,N)" << AAngleconstr.block(0,0,N,N);
    // std::cout << "AAngleconstr.block(0,N,N,N)" << AAngleconstr.block(0,N,N,N);
    // std::cout << "AAngleconstr.block(N,0,N,N)" << AAngleconstr.block(N,0,N,N);
    // std::cout << "AAngleconstr.block(N,N,N,N)" << AAngleconstr.block(N,N,N,N);


            // std::cout << "AYWAAA" << std::endl;
        bAngleConstrMin.block(0,0,N,1) = _lhs.block(0,0,N,1);
        bAngleConstrMin.block(N,0,N,1) = _lhs.block(0,1,N,1);
        // std::cout << "Eldapapaaaa" << std::endl;
        bAngleConstrMax << _rhs.block(0,0,N,1), _rhs.block(0,1,N,1);
        
        int nConstraintscam = Aeq_cam.rows() + AZmp_cam.rows() + AAngleconstr.rows();
        AConstraintcam.resize(nConstraintscam, 2*N);
        bConstraintMincam.resize(nConstraintscam);
        bConstraintMaxcam.resize(nConstraintscam);

    AConstraintcam    << Aeq_cam, AZmp_cam, AAngleconstr;
    bConstraintMincam << beq_cam, bZmpMin_cam, bAngleConstrMin;
    bConstraintMaxcam << beq_cam, bZmpMax_cam, bAngleConstrMax;

}
else{
    // Stack the constraint matrices
    // *****************************
        int nConstraintscam = Aeq_cam.rows() + AZmp_cam.rows();
        AConstraintcam.resize(nConstraintscam, 2*N);
        bConstraintMincam.resize(nConstraintscam);
        bConstraintMaxcam.resize(nConstraintscam);

    AConstraintcam    << Aeq_cam, AZmp_cam;
    bConstraintMincam << beq_cam, bZmpMin_cam;
    bConstraintMaxcam << beq_cam, bZmpMax_cam;
}
    // std::cout << AZmp_cam;

    // Construct the cost function
    // ***************************

    // Construct the H matrix, which is made of two of the same halfH block
        costFunctionHcam.block(0,0,N,N) = qZd_cam*Eigen::MatrixXd::Identity(N,N);
        costFunctionHcam.block(N,N,N,N) = qZd_cam*Eigen::MatrixXd::Identity(N,N);

    // Construct the F vector
    Eigen::VectorXd Nzeros = Eigen::VectorXd::Zero(N);
    costFunctionFcam << Nzeros, Nzeros;

    // Solve QP and update state
    // *************************

    Eigen::VectorXd decisionVariables;
    decisionVariables = solveQP_hpipm(costFunctionHcam, costFunctionFcam, AConstraintcam, bConstraintMincam, bConstraintMaxcam);


    // Split the QP solution in ZMP dot and footsteps
    Eigen::VectorXd zDotOptimalX_cam(N);
    Eigen::VectorXd zDotOptimalY_cam(N);

    zDotOptimalX_cam = (decisionVariables.head(N));
    zDotOptimalY_cam = (decisionVariables.segment(N,N));

    // Update the com-torso state based on the result of the QP
    double ch = cosh(omega*controlTimeStep);
    double sh = sinh(omega*controlTimeStep);

    Eigen::Matrix3d A_upd = Eigen::MatrixXd::Zero(3,3);
    Eigen::Vector3d B_upd = Eigen::VectorXd::Zero(3);
    A_upd<<ch,sh/omega,1-ch,omega*sh,ch,-omega*sh,0,0,1;
    B_upd<<controlTimeStep-sh/omega,1-ch,controlTimeStep;

    Eigen::Vector3d currentStateX_cam = Eigen::Vector3d(current_cam.comPos(0), current_cam.comVel(0), current_cam.zmpPos(0));
    Eigen::Vector3d currentStateY_cam = Eigen::Vector3d(current_cam.comPos(1), current_cam.comVel(1), current_cam.zmpPos(1));
    Eigen::Vector3d nextStateX_cam = A_upd*currentStateX_cam + B_upd*zDotOptimalX_cam(0);
    Eigen::Vector3d nextStateY_cam = A_upd*currentStateY_cam + B_upd*zDotOptimalY_cam(0);

    State next_cam = current_cam;
    next_cam.comPos = Eigen::Vector3d(nextStateX_cam(0), nextStateY_cam(0), comTargetHeight);
    next_cam.comVel = Eigen::Vector3d(nextStateX_cam(1), nextStateY_cam(1), 0.0);
    next_cam.zmpPos = Eigen::Vector3d(nextStateX_cam(2), nextStateY_cam(2), 0.0);
    next_cam.comAcc = omega*omega * (next_cam.comPos - next_cam.zmpPos);
    next_cam.torsoOrient = Eigen::Vector3d(0.0, 0.0, 0.0);

    // std::cout << "current_cam.zmpPos(0) = " << current_cam.zmpPos(0) << std::endl;
    // std::cout << "next_cam.zmpPos(0) = " << next_cam.zmpPos(0) << std::endl;

    // std::cout << "xzcam_max = " << xzcam_max << std::endl;

    xz_dot_cam = zDotOptimalX_cam(0);
    yz_dot_cam = zDotOptimalY_cam(0);

    // Eigen::Vector4d footstepPredicted;
    // footstepPredicted << footstepsOptimalX(0),footstepsOptimalY(0),0.0,predictedOrientations(1);

    //std::cout << "x "<< footstepsOptimalX(0) <<std::endl;
    //std::cout << "y "<< footstepsOptimalX(1) <<std::endl;

    // next_cam.torsoOrient(2) = wrapToPi((next_cam.leftFootOrient(2) + next_cam.rightFootOrient(2)) / 2.0);

    old_fsCount = fsCount;
    ct = ct + 1;

return next_cam;
}