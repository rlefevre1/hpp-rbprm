//
// Copyright (c) 2017 CNRS
// Authors: Steve Tonneau
//
// This file is part of hpp-rbprm
// hpp-core is free software: you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// hpp-core is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.  You should have
// received a copy of the GNU Lesser General Public License along with
// hpp-core  If not, see
// <http://www.gnu.org/licenses/>.

#include <hpp/rbprm/projection/projection.hh>
#include <hpp/rbprm/interpolation/interpolation-constraints.hh>




namespace hpp {
namespace rbprm {
namespace projection{

std::vector<bool> setMaintainRotationConstraints()
{
    std::vector<bool> res;
    for(std::size_t i =0; i <3; ++i)
        res.push_back(true);
    return res;
}

void CreateContactConstraints(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const hpp::rbprm::State& currentState, core::ConfigProjectorPtr_t proj)
{
    model::DevicePtr_t device = fullBody->device_;
    std::vector<bool> cosntraintsR = setMaintainRotationConstraints();
    std::queue<std::string> fixed = currentState.contactOrder_;
    while(!fixed.empty())
    {
        const std::string effector = fixed.front();
        fixed.pop();
        RbPrmLimbPtr_t limb = fullBody->GetLimbs().at(effector);
        const fcl::Vec3f& ppos  = currentState.contactPositions_.at(effector);
        JointPtr_t effectorJoint = device->getJointByName(limb->effector_->name());
        proj->add(core::NumericalConstraint::create (
                                constraints::deprecated::Position::create("",device,
                                                              effectorJoint,fcl::Vec3f(0,0,0), ppos)));
        if(limb->contactType_ == hpp::rbprm::_6_DOF)
        {
            const fcl::Matrix3f& rotation = currentState.contactRotation_.at(effector);
            proj->add(core::NumericalConstraint::create (constraints::deprecated::Orientation::create("", device,
                                                                              effectorJoint,
                                                                              rotation,
                                                                              cosntraintsR)));
        }
    }
}

void CreateRootPosConstraint(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const fcl::Vec3f& target, core::ConfigProjectorPtr_t proj)
{
    proj->add(core::NumericalConstraint::create (
                            constraints::deprecated::Position::create("",fullBody->device_,
                                                          fullBody->device_->rootJoint(),fcl::Vec3f(0,0,0), target)));
}

ProjectionReport projectToRootPosition(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const fcl::Vec3f& target,
                                           const hpp::rbprm::State& currentState)
{
    ProjectionReport res;
    core::ConfigProjectorPtr_t proj = core::ConfigProjector::create(fullBody->device_,"proj", 0.001, 40);
    CreateContactConstraints(fullBody, currentState, proj);
    CreateRootPosConstraint(fullBody, target, proj);
    model::Configuration_t configuration = currentState.configuration_;
    res.success_ = proj->apply(configuration);
    res.result_ = currentState;
    res.result_.configuration_ = configuration;
    return res;
}


void LockFromRoot(hpp::model::DevicePtr_t device, model::ConfigurationIn_t targetRootConfiguration, core::ConfigProjectorPtr_t& projector)
{
    model::JointPtr_t cJoint = device->rootJoint();
    std::size_t currentLength(0);
    core::size_type rankInConfiguration;
    while(currentLength<=targetRootConfiguration.rows())
    {
        rankInConfiguration = (cJoint->rankInConfiguration ());
        projector->add(core::LockedJoint::create(cJoint,targetRootConfiguration.segment(rankInConfiguration, cJoint->configSize())));
        currentLength += cJoint->configSize();
    }
}


ProjectionReport projectToRootConfiguration(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const model::ConfigurationIn_t conf,
                                           const hpp::rbprm::State& currentState)
{
    ProjectionReport res;
    core::ConfigProjectorPtr_t proj = core::ConfigProjector::create(fullBody->device_,"proj", 0.001, 40);
    CreateContactConstraints(fullBody, currentState, proj);
    LockFromRoot(fullBody->device_, conf, proj);
    model::Configuration_t configuration = currentState.configuration_;
    res.success_ = proj->apply(configuration);
    res.result_ = currentState;
    res.result_.configuration_ = configuration;
    return res;
}

ProjectionReport setCollisionFree(hpp::rbprm::RbPrmFullBodyPtr_t fullBody, const std::string& limbName,const hpp::rbprm::State& currentState)
{
    ProjectionReport res;
    res.result_ = currentState;
    model::Configuration_t configuration = currentState.configuration_;
    RbPrmLimbPtr_t limb = fullBody->GetLimbs().at(limbName);
    for(sampling::SampleVector_t::const_iterator cit = limb->sampleContainer_.samples_.begin();
        cit != limb->sampleContainer_.samples_.end(); ++cit)
    {
        hpp::core::ValidationReportPtr_t valRep (new hpp::core::CollisionValidationReport);
        if(validation->validate(configuration, valRep) )
        {
            res.result_.configuration_ = configuration;
            res.success_ = true;
        }
        // load after to test current configuraiton (so miss the last configuration but that s probably okay..)
        sampling::Load(*cit, configuration);
    }
    return res;
}

} // namespace projection
} // namespace rbprm
} // namespace hpp
