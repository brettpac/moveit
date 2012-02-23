/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan, Jia Pan */

#include "collision_detection/fcl/collision_common.h"
#include <fcl/geometric_shape_to_BVH_model.h>
#include <ros/console.h>

namespace collision_detection
{
  bool collisionCallback(fcl::CollisionObject* o1, fcl::CollisionObject* o2, void *data)
  {
    CollisionData *cdata = (CollisionData*)data;
    if (cdata->done_)
      return true;
    const CollisionGeometryData *cd1 = static_cast<const CollisionGeometryData*>(o1->getCollisionGeometry()->getUserData());
    const CollisionGeometryData *cd2 = static_cast<const CollisionGeometryData*>(o2->getCollisionGeometry()->getUserData());
    
    // use the collision matrix (if any) to avoid certain collision checks
    DecideContactFn dcf;
    bool always_allow_collision = false;
    if (cdata->acm_)
    {
      AllowedCollision::Type type;
      bool found = cdata->acm_->getAllowedCollision(cd1->getID(), cd2->getID(), type);
      if (found)
      {
	// if we have an entry in the collision matrix, we read it
	if (type == AllowedCollision::ALWAYS)
	{
	  always_allow_collision = true;
	  if (cdata->req_->verbose)
	    ROS_INFO("Collision between '%s' and '%s' is always allowed. No contacts are computed.",
		     cd1->getID().c_str(), cd2->getID().c_str());
	}
	else
	  if (type == AllowedCollision::CONDITIONAL)
	  {
	    cdata->acm_->getAllowedCollision(cd1->getID(), cd2->getID(), dcf);
	    if (cdata->req_->verbose)
	      ROS_INFO("Collision between '%s' and '%s' is conditionally allowed", cd1->getID().c_str(), cd2->getID().c_str());
	  }
      }
    }
    
    // check if a link is touching an attached object
    if (cd1->type == BodyTypes::ROBOT_LINK && cd2->type == BodyTypes::ROBOT_ATTACHED)
    {
      if (cd2->ptr.ab->touch_links_.find(cd1->getID()) != cd2->ptr.ab->touch_links_.end())
      {
	always_allow_collision = true;
	if (cdata->req_->verbose)
	  ROS_INFO("Robot link '%s' is allowed to touch attached object '%s'. No contacts are computed.",
		   cd1->getID().c_str(), cd2->getID().c_str());
      }
    }
    else
      if (cd2->type == BodyTypes::ROBOT_LINK && cd1->type == BodyTypes::ROBOT_ATTACHED)
      {
	if (cd1->ptr.ab->touch_links_.find(cd2->getID()) != cd1->ptr.ab->touch_links_.end())
	{
	  always_allow_collision = true;
	  if (cdata->req_->verbose)
	    ROS_INFO("Robot link '%s' is allowed to touch attached object '%s'. No contacts are computed.",
		     cd2->getID().c_str(), cd1->getID().c_str());
	}
      }
    
    // if collisions are always allowed, we are done
    if (always_allow_collision)
      return false;
    
    // see if we need to compute a contact
    std::size_t want_contact_count = 0;
    if (cdata->req_->contacts)
      if (cdata->res_->contact_count < cdata->req_->max_contacts)
      {
	std::size_t have;
	if (cd1->getID() < cd2->getID())
	{
	  std::pair<std::string, std::string> cp(cd1->getID(), cd2->getID());
	  have = cdata->res_->contacts.find(cp) != cdata->res_->contacts.end() ? cdata->res_->contacts[cp].size() : 0;
	}
	else
	{
	  std::pair<std::string, std::string> cp(cd2->getID(), cd1->getID());
	  have = cdata->res_->contacts.find(cp) != cdata->res_->contacts.end() ? cdata->res_->contacts[cp].size() : 0;
	}
	if (have < cdata->req_->max_contacts_per_pair)
	  want_contact_count = cdata->req_->max_contacts_per_pair - have;
      }
    
    if (dcf)
    {
      // if we have a decider for allowed contacts, we need to look at all the contacts
      bool exhaustive = true;
      bool enable_contact = true;
      std::vector<fcl::Contact> contacts;
      int num_contacts = fcl::collide(o1, o2, std::numeric_limits<int>::max(), exhaustive, enable_contact, contacts);
      if (num_contacts > 0)
      {
	if (cdata->req_->verbose)
	  ROS_INFO("Found %d contacts between '%s' and '%s'. These contacts will be evaluated to check if they are accepted or not",
		   num_contacts, cd1->getID().c_str(), cd2->getID().c_str());
	Contact c;
	const std::pair<std::string, std::string> &pc = cd1->getID() < cd2->getID() ?
	  std::make_pair(cd1->getID(), cd2->getID()) : std::make_pair(cd2->getID(), cd1->getID());
	for (int i = 0 ; i < num_contacts ; ++i)
	{
	  fcl2contact(contacts[i], c);
	  // if the contact is  not allowed, we have a collision
	  if (dcf(c) == false)
	  {
	    // store the contact, if it is needed
	    if (want_contact_count > 0)
	    {
	      --want_contact_count;
	      cdata->res_->contacts[pc].push_back(c);
	      cdata->res_->contact_count++;
	      ROS_INFO("Found unacceptable contact between '%s' and '%s'. Contact was stored.", cd1->getID().c_str(), cd2->getID().c_str());
	    }
	    else
	      ROS_INFO("Found unacceptable contact between '%s' and '%s'. Contact was not stored.", cd1->getID().c_str(), cd2->getID().c_str());
	    cdata->res_->collision = true;
	    if (want_contact_count == 0)
	      break;
	  }
	}
      }
    }
    else
    {
      if (want_contact_count > 0)
      {
	// otherwise, we need to compute more things
	bool exhaustive = false;
	bool enable_contact = true;
	std::vector<fcl::Contact> contacts;
	int num_contacts = fcl::collide(o1, o2, want_contact_count, exhaustive, enable_contact, contacts);
	if (num_contacts > 0)
	{
	  // make sure we don't get more contacts than we want
	  if (want_contact_count >= (std::size_t)num_contacts)
	    want_contact_count -= num_contacts;
	  else
	  {
	    num_contacts = want_contact_count;
	    want_contact_count = 0;
	  }
	  
	  if (cdata->req_->verbose)
	    ROS_INFO("Found %d contacts between '%s' and '%s', which constitute a collision. %d contacts will be stored",
		     num_contacts, cd1->getID().c_str(), cd2->getID().c_str(), (int)num_contacts);
	  const std::pair<std::string, std::string> &pc = cd1->getID() < cd2->getID() ?
	    std::make_pair(cd1->getID(), cd2->getID()) : std::make_pair(cd2->getID(), cd1->getID());
	  cdata->res_->collision = true;
	  for (int i = 0 ; i < num_contacts ; ++i)
	  {
	    Contact c;
	    fcl2contact(contacts[i], c);
	    cdata->res_->contacts[pc].push_back(c);
	    cdata->res_->contact_count++;
	  }
	}
      }
      else
      {
	bool exhaustive = false;
	bool enable_contact = false;
	std::vector<fcl::Contact> contacts;
	int num_contacts = fcl::collide(o1, o2, 1, exhaustive, enable_contact, contacts);
	if (num_contacts > 0)
	{
	  cdata->res_->collision = true;
	  if (cdata->req_->verbose)
	    ROS_INFO("Found a contact between '%s' and '%s', which constitutes a collision. Contact information is not stored.",
		     cd1->getID().c_str(), cd2->getID().c_str());
	}
      }
    }
    
    if (cdata->res_->collision)
      if (!cdata->req_->contacts || cdata->res_->contact_count >= cdata->req_->max_contacts)
      {
	cdata->done_ = true;
	if (cdata->req_->verbose)
	  ROS_INFO("Collision checking is considered complete (collision was found and %d contacts are stored)",
		   (unsigned int)cdata->res_->contact_count);
      }
    
    return cdata->done_;
  }
  
  boost::shared_ptr<fcl::CollisionGeometry> createCollisionGeometry(const shapes::StaticShape *shape)
  {
    fcl::CollisionGeometry* g = NULL;
    switch (shape->type)
    {
    case shapes::PLANE:
      {
	const shapes::Plane *p = static_cast<const shapes::Plane*>(shape);
	g = new fcl::Plane(p->a, p->b, p->c, p->d);
      }
      break;
    default:
      ROS_FATAL("This shape type (%d) is not supported using FCL yet", (int)shape->type);
    }
    if (g)
      g->computeLocalAABB();
    return boost::shared_ptr<fcl::CollisionGeometry>(g);
  }
  
  boost::shared_ptr<fcl::CollisionGeometry> createCollisionGeometry(const shapes::Shape *shape, double scale, double padding)
  {
    if (fabs(scale - 1.0) <= std::numeric_limits<double>::epsilon() && fabs(padding) <= std::numeric_limits<double>::epsilon())
      return createCollisionGeometry(shape);
    else
    {
      boost::scoped_ptr<shapes::Shape> scaled_shape(shape->clone());
      scaled_shape->scaleAndPadd(scale, padding);
      return createCollisionGeometry(scaled_shape.get());
    }
  }
  
  boost::shared_ptr<fcl::CollisionGeometry> createCollisionGeometry(const shapes::Shape *shape)
  {
    fcl::BVHModel<fcl::OBB>* g = new fcl::BVHModel<fcl::OBB>();
    
    switch (shape->type)
    {
    case shapes::SPHERE:
      {
	fcl::generateBVHModel(*g, fcl::Sphere(static_cast<const shapes::Sphere*>(shape)->radius));
      }
      break;
    case shapes::BOX:
      {
	const double *size = static_cast<const shapes::Box*>(shape)->size;
	fcl::generateBVHModel(*g, fcl::Box(size[0], size[1], size[2]));
      }
      break;
    case shapes::CYLINDER:
      {
	fcl::generateBVHModel(*g, fcl::Cylinder(static_cast<const shapes::Cylinder*>(shape)->radius,
						static_cast<const shapes::Cylinder*>(shape)->length));
      }
      break;
    case shapes::MESH:
      {
	const shapes::Mesh *mesh = static_cast<const shapes::Mesh*>(shape);
	if (mesh->vertex_count > 0 && mesh->triangle_count > 0)
	{
	  std::vector<fcl::Triangle> tri_indices(mesh->triangle_count);
	  for(unsigned int i = 0; i < mesh->triangle_count; ++i)
	    tri_indices[i] = fcl::Triangle(mesh->triangles[3 * i], mesh->triangles[3 * i + 1], mesh->triangles[3 * i + 2]);
	  
	  std::vector<fcl::Vec3f> points(mesh->vertex_count);
	  for (unsigned int i = 0; i < mesh->vertex_count; ++i)
	    points[i] = fcl::Vec3f(mesh->vertices[3 * i], mesh->vertices[3 * i + 1], mesh->vertices[3 * i + 2]);
	  
	  g->beginModel();
	  g->addSubModel(points, tri_indices);
	  g->endModel();
	}
      }
      break;
    default:
      ROS_FATAL("This shape type (%d) is not supported using FCL yet", (int)shape->type);
    }
    if (g)
      g->computeLocalAABB();
    return boost::shared_ptr<fcl::CollisionGeometry>(g);
  } 

  void transform2fcl(const Eigen::Affine3d &b, fcl::SimpleTransform &f)
  {
    Eigen::Quaterniond q(b.rotation());
    f.setTranslation(fcl::Vec3f(b.translation().x(), b.translation().y(), b.translation().z()));
    f.setQuatRotation(fcl::SimpleQuaternion(q.w(), q.x(), q.y(), q.z()));
  }
  fcl::SimpleTransform transform2fcl(const Eigen::Affine3d &b)
  {
    fcl::SimpleTransform t;
    transform2fcl(b, t);
    return t;
  }
  void fcl2contact(const fcl::Contact &fc, Contact &c)
  {
    c.pos = Eigen::Vector3d(fc.pos[0], fc.pos[1], fc.pos[2]);
    c.normal = Eigen::Vector3d(fc.normal[0], fc.normal[1], fc.normal[2]);
    c.depth = fc.penetration_depth;
    const CollisionGeometryData *cgd1 = static_cast<const CollisionGeometryData*>(fc.o1->getUserData());
    c.body_name_1 = cgd1->getID();
    c.body_type_1 = cgd1->type;
    const CollisionGeometryData *cgd2 = static_cast<const CollisionGeometryData*>(fc.o2->getUserData());
    c.body_name_2 = cgd2->getID();
    c.body_type_2 = cgd2->type;
  }
}

void collision_detection::FCLObject::registerTo(fcl::BroadPhaseCollisionManager *manager)
{
  for (std::size_t i = 0 ; i < collision_objects_.size() ; ++i)
    manager->registerObject(collision_objects_[i].get());
}

void collision_detection::FCLObject::unregisterFrom(fcl::BroadPhaseCollisionManager *manager)
{
  for (std::size_t i = 0 ; i < collision_objects_.size() ; ++i)
    manager->unregisterObject(collision_objects_[i].get());
}

void collision_detection::FCLObject::clear(void)
{
  collision_objects_.clear();
  collision_geometry_data_.clear();
}
