//
// Copyright (c) 2016 CNRS
// Authors: Mylene Campana
//
// This file is part of hpp-core
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

#include <hpp/util/debug.hh>
#include <hpp/model/configuration.hh>
#include <hpp/model/device.hh>
#include <hpp/core/basic-configuration-shooter.hh>
#include <hpp/core/connected-component.hh>
#include <hpp/core/node.hh>
#include <hpp/core/edge.hh>
#include <hpp/core/path.hh>
#include <hpp/core/problem.hh>
#include <hpp/core/roadmap.hh>
#include <hpp/rbprm/rbprm-state.hh>
#include <hpp/rbprm/fullbodyBallistic/ballistic-planner.hh>
#include <polytope/stability_margin.h>
#include "utils/algorithms.h"

#include <hpp/core/path-validation-report.hh>
#include <hpp/rbprm/rbprm-path-validation.hh>
#include <hpp/rbprm/rbprm-validation-report.hh>
#include <hpp/rbprm/fullbodyBallistic/parabola-library.hh>
#include <hpp/core/config-validations.hh>

namespace hpp {
  namespace rbprm {
    using model::displayConfig;
    using core::value_type;
    using core::vector_t;
    using model::size_type;

    BallisticPlanner::BallisticPlanner (const core::Problem& problem):
      PathPlanner (problem), problem_ (core::ProblemPtr_t(&problem)),
      configurationShooter_ (problem.configurationShooter()),
      smParabola_(rbprm::SteeringMethodParabola::create((core::ProblemPtr_t(&problem)))),
      rbRoadmap_(core::RbprmRoadmap::create (problem.distance (),problem.robot())), roadmap_(boost::dynamic_pointer_cast<core::Roadmap>(rbRoadmap_)),
      fullRobot_ (RbPrmFullBody::create(problem.robot ())),
      contactSize_ (core::vector_t(2))
    {
      hppDout(notice,"Constructor ballistic-planner");
    }

    BallisticPlanner::BallisticPlanner (const core::Problem& problem,
					const core::RoadmapPtr_t& roadmap) :
      PathPlanner (problem, roadmap), problem_ (core::ProblemPtr_t(&problem)),
      configurationShooter_ (problem.configurationShooter()),
      smParabola_(rbprm::SteeringMethodParabola::create((core::ProblemPtr_t(&problem)))),
      rbRoadmap_(boost::dynamic_pointer_cast<core::RbprmRoadmap>(roadmap)),
      roadmap_(roadmap), fullRobot_ (RbPrmFullBody::create(problem.robot ())),
      contactSize_ (core::vector_t(2))
    {
      hppDout(notice,"Constructor ballistic-planner with Roadmap");
      hppDout(info,"contactSize_= " << contactSize_);
      if (!rbRoadmap_) {
	hppDout (info, "Problem with RbPrmRoadmap cast, create new one");
	rbRoadmap_ = core::RbprmRoadmap::create (problem.distance (),problem.robot());
      }
      
    }

    void BallisticPlanner::oneStep ()
    {
      core::DevicePtr_t robot (problem ().robot ());
      core::PathPtr_t localPath;
      DelayedEdge_t fwdDelayedEdge, bwdDelayedEdge;
      DelayedEdges_t fwdDelayedEdges;
      const size_type indexECS = robot->configSize () - robot->extraConfigSpace ().dimension ();

      // shoot a RB-valid random configuration using rbprm-shooter
      core::ConfigurationPtr_t q_rand;
      bool valid = false;
      core::ValidationReportPtr_t report;
      
      while (!valid) {
      hppDout(notice,"# oneStep BEGIN");
      q_rand = configurationShooter_->shoot ();
      hppDout (info, "q_rand: " << displayConfig (*q_rand));
      computeGIWC(*q_rand);
      for (std::size_t i = 0; i < 3; i++)
      (*q_rand) [i + indexECS] = contactNormalAverage_ [i];
	hppDout (info, "q_rand after giwc: " << displayConfig (*q_rand));
	*q_rand = setOrientation (robot, *q_rand); // update with new normal
	hppDout (info, "q_rand after setOrient: " << displayConfig (*q_rand));
	valid = problem ().configValidations()->validate(*q_rand,report);
	if (!valid)
	  hppDout (info, "giwc normal + setOrientation => not valid");
      }

      // Add q_rand as a new node: here for the parabola, as the impact node
      core::NodePtr_t impactNode = roadmap ()->addNode (q_rand);
      impactNode->indexInRM (roadmap ()->nodeIndex_);
      roadmap ()->nodeIndex_++;
      //rbprmRoadmap()->addNode(q_rand);

      // try to connect the random configuration to each connected component
      // of the roadmap.
      for (core::ConnectedComponents_t::const_iterator itcc =
	     roadmap ()->connectedComponents ().begin ();
	   itcc != roadmap ()->connectedComponents ().end (); ++itcc) {
	core::ConnectedComponentPtr_t cc = *itcc;
	// except its own connected component of course
	if (cc != impactNode->connectedComponent ()) {

	  // iteration on each node of the current connected-component
	  for (core::NodeVector_t::const_iterator n_it = cc->nodes ().begin (); 
	       n_it != cc->nodes ().end (); ++n_it){
	    core::ConfigurationPtr_t qCC = (*n_it)->configuration ();
	    hppDout (info, "qCC: " << displayConfig (*qCC));

	    // Create forward local path from qCC to q_rand
	    localPath = (*smParabola_) (*qCC, *q_rand);

	    // if a forward path is returned, it is valid
	    if (localPath) {
	      // Save forward & backward delayed edges
	      fwdDelayedEdge = DelayedEdge_t (*n_it, impactNode, localPath);
	      fwdDelayedEdges.push_back (fwdDelayedEdge);
		
	      // Assuming that SM is symmetric (V0max = Vfmax)
	      // WARN: I had to reverse *n_it - impNode HERE
	      // to add edges consecutively to same vector fwdDelayedEdges
	      bwdDelayedEdge = DelayedEdge_t (impactNode, *n_it,
					      localPath->reverse ());
	      fwdDelayedEdges.push_back (bwdDelayedEdge);
	    } //if SM has returned a non-empty path
	  }//for nodes in cc
	}//avoid impactNode cc
      }//for cc in roadmap

      // Insert in roadmap all forward delayed edges (DE)
      bool avoidAddIdenticalEdge = true;
      for (DelayedEdges_t::const_iterator itEdge = fwdDelayedEdges.begin ();
	   itEdge != fwdDelayedEdges.end (); ++itEdge) {
	const core::NodePtr_t& nodeDE = itEdge-> get <0> ();
	const core::NodePtr_t& node2DE = itEdge-> get <1> ();
	const core::PathPtr_t& pathDE = itEdge-> get <2> ();
	core::EdgePtr_t edge = roadmap ()->addEdge (nodeDE, node2DE, pathDE);
	hppDout(info, "connection between q1: " 
		<< displayConfig (*(nodeDE->configuration ()))
		<< "and q2: "
		<< displayConfig (*(node2DE->configuration ())));
	edge->indexInRM (roadmap ()->edgeIndex_);
	// assure that forward and backward edges have same edgeIndex
	if (!avoidAddIdenticalEdge) {
	  roadmap ()->edgeIndex_++;
	  avoidAddIdenticalEdge = true;
	} else
	  avoidAddIdenticalEdge = false;
      }
    }

    void BallisticPlanner::tryDirectPath ()
    {
      // call steering method here to build a direct conexion
      core::PathPtr_t path;
      std::vector<std::string> filter;
      core::NodePtr_t initNode = roadmap ()->initNode();
      for (core::Nodes_t::const_iterator itn = roadmap ()->goalNodes ().begin();itn != roadmap ()->goalNodes ().end (); ++itn) {
        core::ConfigurationPtr_t q1 ((initNode)->configuration ());
        core::ConfigurationPtr_t q2 ((*itn)->configuration ());
        assert (*q1 != *q2);
        path = (*smParabola_) (*q1, *q2);
        if (path) { // has no collision
	  hppDout(notice, "#### direct parabola path is valid !");
	  roadmap ()->addEdge (initNode, *itn, path);
	  roadmap ()->addEdge (*itn, initNode, path->reverse());
	} else {
	  hppDout(notice, "#### direct parabola path not valid !");
	}
      } //for qgoals
    }

    void BallisticPlanner::computeGIWC(const core::Configuration_t q){
      hppDout(notice,"## compute GIWC");
      const polytope::ProjectedCone* giwc = NULL;
      core::ValidationReportPtr_t report;
      const core::DevicePtr_t& robot (problem_->robot ());
      model::RbPrmDevicePtr_t rbDevice =
	boost::dynamic_pointer_cast<model::RbPrmDevice> (robot);
      if (!rbDevice) {
	hppDout(error,"~~ Device cast in RB problem");
	return;
      }

      const bool isValid = problem ().configValidations()->validate(q,report);
      if(!isValid) {
	hppDout(warning,"~~ ComputeGIWC : config is not valid");
	return;
      }
      if (!report) {
	hppDout(error,"~~ Report problem");
	return;
      }
      core::RbprmValidationReportPtr_t rbReport =
	boost::dynamic_pointer_cast<core::RbprmValidationReport> (report);
      // checks :
      if(!rbReport)
	{
	  hppDout(error,"~~ Validation Report cannot be cast");
	  return;
	}
      
      //TODO
      polytope::T_rotation_t rotContact(3*rbReport->ROMReports.size(),3);
      polytope::vector_t posContact(3*rbReport->ROMReports.size());
      
      
      // get the 2 object in contact for each ROM :
      hppDout(info,"~~ Number of roms in collision : "<<rbReport->ROMReports.size());
      fcl::Vec3f normalAv (0,0,0);
      const std::size_t nbNormalAv = rbReport->ROMReports.size();
      size_t indexRom = 0;
      for(std::map<std::string,core::CollisionValidationReportPtr_t>::const_iterator it = rbReport->ROMReports.begin() ; it != rbReport->ROMReports.end() ; ++it)
	{
	  hppDout(info,"~~ for rom : "<<it->first);
	  core::CollisionObjectPtr_t obj1 = it->second->object1;
	  core::CollisionObjectPtr_t obj2 = it->second->object2;
	  hppDout(notice,"~~ collision between : "<<obj1->name() << " and "<<obj2->name());
	  fcl::CollisionResult result = it->second->result;
	  // result is the object colliding with the current ROM
	  /* size_t numContact =result.numContacts();
	     hppDout(notice,"~~ number of contact : "<<numContact);
	     std::ostringstream ss;
	     ss<<"[";
	     for(size_t i = 0 ; i < numContact ; i++)
	     { // print with python formating :
	     ss<<"["<<result.getContact(i).pos[0]<<","<<result.getContact(i).pos[1]<<","<<result.getContact(i).pos[2]<<"]";
	     if(i< (numContact-1))
             ss<<",";
	     }
	     ss<<"]";
	     std::cout<<ss.str()<<std::endl;
	  */
        
	  // get intersection between the two objects :
	  obj1->fcl();
	  geom::T_Point vertices1;
	  geom::BVHModelOBConst_Ptr_t model1 =  geom::GetModel(obj1->fcl());
	  hppDout(info,"vertices obj1 : "<<obj1->name()<< " ( "<<model1->num_vertices<<" ) ");
	  for(int i = 0 ; i < model1->num_vertices ; ++i)
	    {
	      vertices1.push_back(Eigen::Vector3d(model1->vertices[i][0], model1->vertices[i][1], model1->vertices[i][2]));
	      //hppDout(notice,"vertices : "<<model1->vertices[i]);
	    }
	  //std::cout<<ss1.str()<<std::endl;
        
        
	  obj2->fcl();
	  geom::T_Point vertices2;
	  geom::BVHModelOBConst_Ptr_t model2 =  geom::GetModel(obj2->fcl());
	  hppDout(info,"vertices obj2 : "<<obj2->name()<< " ( "<<model2->num_vertices<<" ) ");
	  for(int i = 0 ; i < model2->num_vertices ; ++i)
	    {
	      vertices2.push_back(Eigen::Vector3d(model2->vertices[i][0], model2->vertices[i][1], model2->vertices[i][2]));
	      // hppDout(notice,"vertices : "<<model2->vertices[i]);
	    }
	  //std::cout<<ss2.str()<<std::endl;
        
	  geom::T_Point hull = geom::intersectPolygonePlane(model1,model2,fcl::Vec3f(0,0,1),geom::ZJUMP,result);
	  
	  if(hull.size() == 0){
	    hppDout(error,"No intersection between rom and environnement");
	    return;
	  }

	  // todo : compute center point of the hull
	  polytope::vector3_t normal,tangent0,tangent1;
	  geom::Point center = geom::center(hull.begin(),hull.end());
	  posContact.segment<3>(indexRom*3) = center;
	  //std::cout<<center<<std::endl<<std::endl;
	  polytope::rotation_t rot; 
	  normal = -result.getContact(0).normal; // of contact surface
	  hppDout(notice," !!! normal for GIWC : "<<normal);
	  // compute tangent vector : 
	  tangent0 = normal.cross(polytope::vector3_t(1,0,0));
	  if(tangent0.dot(tangent0)<0.001)
	    tangent0 = normal.cross(polytope::vector3_t(0,1,0)); 
	  tangent1 = normal.cross(tangent0);
	  rot(0,0) = tangent0(0) ; rot(0,1) = tangent1(0) ; rot(0,2) = normal(0);
	  rot(1,0) = tangent0(1) ; rot(1,1) = tangent1(1) ; rot(1,2) = normal(1);
	  rot(2,0) = tangent0(2) ; rot(2,1) = tangent1(2) ; rot(2,2) = normal(2);
        
	  rotContact.block<3,3>(indexRom*3,0) = rot;
	  //std::cout<<rot<<std::endl<<std::endl;
        
	  indexRom++;
	  for (std::size_t i = 0; i < 3; i++) {
	    normalAv [i] += normal [i]/nbNormalAv;
	    hppDout (info, "normal [i]/nbNormalAv= " << normal [i]/nbNormalAv);
	  }
	} // for each ROMS
      hppDout (info, "normalAv= " << normalAv);
      normalAv.normalize ();
      hppDout (info, "normed normalAv= " << normalAv);
      contactNormalAverage_ = normalAv;
      
      polytope::vector_t x(rbReport->ROMReports.size());
      polytope::vector_t y(rbReport->ROMReports.size());
      polytope::vector_t nu(rbReport->ROMReports.size());
      const value_type xContact = rbDevice->contactSize_ [0];
      const value_type yContact = rbDevice->contactSize_ [1];
      hppDout (info, "xContact= " << xContact);
      hppDout (info, "yContact= " << yContact);
      for(size_t k = 0 ; k<rbReport->ROMReports.size() ; ++k){
        x(k) = xContact; // approx size of foot  (x length, y width)
        y(k) = yContact; 
        nu(k) = problem_->mu_;
      }
      // save giwc in node structure
      // PROBLEM: when activating polytope::U_stance,
      // 'rand' in config-shooter always return the same values
      //giwc = polytope::U_stance (rotContact, posContact, nu, x, y);
      //hppDout (info, "giwc was computed");
      //const core::matrix_t& V = giwc->V;
    }// computeGIWC

  } // namespace core
} // namespace hpp
