#ifndef IAXIS_H
#define IAXIS_H

class IAxis { 
    public: 
        ~IAxis = default; 
        virtual Result enable() = 0; 
        virtual Result disable() = 0; 
        virtual Result stop() = 0; 
        virtual AxisStatus status() const = 0; 
}; 

class IVelocityAxis : public virtual IAxis { 
    public: 
        virtual Result set_velocity_rpm(int rpm) = 0;     
}; 

class IPositionAxis  : public virtual IAxis { 
    public: 
        virtual Result move_to_mm(double position_mm) = 0; 
}



#endif