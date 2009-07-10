/*************************************************************************
 Copyright (C) 2008 by Bruno Chareyre		                         *
*  bruno.chareyre@hmg.inpg.fr      					 *
*                                                                        *
*  This program is free software; it is licensed under the terms of the  *
*  GNU General Public License v2 or later. See file LICENSE for details. *
*************************************************************************/

#include"NewtonsDampedLaw.hpp"
#include<yade/core/MetaBody.hpp>
#include<yade/pkg-common/RigidBodyParameters.hpp>
#include<yade/lib-base/yadeWm3Extra.hpp>
#include<yade/pkg-dem/Clump.hpp>

YADE_PLUGIN("NewtonsDampedLaw");

void NewtonsDampedLaw::cundallDamp(const Real& dt, const Vector3r& f, const Vector3r& velocity, Vector3r& acceleration, const Vector3r& m, const Vector3r& angularVelocity, Vector3r& angularAcceleration){
	for(int i=0; i<3; i++){
		angularAcceleration[i]*= 1 - damping*Mathr::Sign ( m[i]*(angularVelocity[i] + (Real) 0.5 *dt*angularAcceleration[i]) );
		acceleration       [i]*= 1 - damping*Mathr::Sign ( f[i]*(velocity       [i] + (Real) 0.5 *dt*acceleration       [i]) );
	}
}

void NewtonsDampedLaw::applyCondition ( MetaBody * ncb )
{
	ncb->bex.sync();
	Real dt=Omega::instance().getTimeStep();

	FOREACH(const shared_ptr<Body>& b, *ncb->bodies){
		// clump members are non-dynamic; they skip the rest of loop once their forces are properly taken into account, however
		if (!b->isDynamic && !b->isClumpMember()) continue;
		
		RigidBodyParameters* rb = YADE_CAST<RigidBodyParameters*>(b->physicalParameters.get());
		const body_id_t& id=b->getId();
		const Vector3r& m=ncb->bex.getTorque(id);
		const Vector3r& f=ncb->bex.getForce(id);

		//Newtons mometum law :
		if (b->isStandalone()){
			rb->angularAcceleration=diagDiv(m,rb->inertia);
			rb->acceleration=f/rb->mass;
		}
		else if (b->isClump()){
			// at this point, forces from clump members are already summed up, this is just for forces applied to clump proper, if there are such (does it have some physical meaning?)
			rb->angularAcceleration+=diagDiv(m,rb->inertia);
			rb->acceleration+=f/rb->mass; // accel for clump will be reset in Clump::moveMembers, called from the clump itself
		}
		else {
			assert(b->isClumpMember());
			assert(b->clumpId>b->id);
			const shared_ptr<Body>& clump=Body::byId(b->clumpId,ncb);
			RigidBodyParameters* clumpRBP=YADE_CAST<RigidBodyParameters*> ( clump->physicalParameters.get() );
			Vector3r diffClumpAccel=f/clumpRBP->mass;
			// angular acceleration from: normal torque + torque generated by the force WRT particle centroid on the clump centroid
			Vector3r diffClumpAngularAccel=diagDiv(m,clumpRBP->inertia)+diagDiv((rb->se3.position-clumpRBP->se3.position).Cross(f),clumpRBP->inertia); 
			// damp increment of accels on the clump, using velocities of the clump MEMBER
			cundallDamp(dt,f,rb->velocity,diffClumpAccel,m,rb->angularVelocity,diffClumpAngularAccel);
			// clumpRBP->{acceleration,angularAcceleration} are reset byt Clump::moveMembers, it is ok to just increment here
			clumpRBP->acceleration+=diffClumpAccel;
			clumpRBP->angularAcceleration+=diffClumpAngularAccel;
			maxVelocitySq=max(maxVelocitySq,rb->velocity.SquaredLength());
			continue;
		}

		assert(!b->isClumpMember());
		// damping: applied to non-clumps only, as clumps members were already damped above
		if(!b->isClump()) cundallDamp(dt,f,rb->velocity,rb->acceleration,m,rb->angularVelocity,rb->angularAcceleration);

		maxVelocitySq=max(maxVelocitySq,rb->velocity.SquaredLength());

		// blocking DOFs
		if(rb->blockedDOFs==0){ /* same as: rb->blockedDOFs==PhysicalParameters::DOF_NONE */
			rb->angularVelocity=rb->angularVelocity+dt*rb->angularAcceleration;
			rb->velocity=rb->velocity+dt*rb->acceleration;
		} else {
			if((rb->blockedDOFs & PhysicalParameters::DOF_X)==0) rb->velocity[0]+=dt*rb->acceleration[0];
			if((rb->blockedDOFs & PhysicalParameters::DOF_Y)==0) rb->velocity[1]+=dt*rb->acceleration[1];
			if((rb->blockedDOFs & PhysicalParameters::DOF_Z)==0) rb->velocity[2]+=dt*rb->acceleration[2];
			if((rb->blockedDOFs & PhysicalParameters::DOF_RX)==0) rb->angularVelocity[0]+=dt*rb->angularAcceleration[0];
			if((rb->blockedDOFs & PhysicalParameters::DOF_RY)==0) rb->angularVelocity[1]+=dt*rb->angularAcceleration[1];
			if((rb->blockedDOFs & PhysicalParameters::DOF_RZ)==0) rb->angularVelocity[2]+=dt*rb->angularAcceleration[2];
		}

		Vector3r axis = rb->angularVelocity;
		Real angle = axis.Normalize();
		Quaternionr q;
		q.FromAxisAngle ( axis,angle*dt );
		rb->se3.orientation = q*rb->se3.orientation;
		if(ncb->bex.getMoveRotUsed() && ncb->bex.getRot(id)!=Vector3r::ZERO){ Vector3r r(ncb->bex.getRot(id)); Real norm=r.Normalize(); q.FromAxisAngle(r,norm); rb->se3.orientation=q*rb->se3.orientation; }
		rb->se3.orientation.Normalize();

		rb->se3.position += rb->velocity*dt + ncb->bex.getMove(id);

		if(b->isClump()) static_cast<Clump*>(b.get())->moveMembers();
	}
}

/*
:09:37] eudoxos2: enum {LOOP1,LOOP2,END}
[16:09:37] eudoxos2: for(int state=LOOP1; state!=END; state++){
[16:09:37] eudoxos2: 	FOREACH(const shared_ptr<Body>& b, rootBody->bodies){
[16:09:38] eudoxos2: 		if(b->isClumpMember() && LOOP1){ [[apply that on b->clumpId]]  }
[16:09:38] eudoxos2: 		if((b->isStandalone && LOOP1) || (b->isClump && LOOP2){ [[damping, newton, integrate]]; b->moveClumpMembers(); }
[16:09:40] eudoxos2: 		}
[16:09:42] eudoxos2: 	}
[16:09:44] eudoxos2: }*/


