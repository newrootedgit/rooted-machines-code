#ifndef ILOGGER_H
#define ILOGGER_H

struct motorData { 
    float torque; 
    float temperature; 
}; 

class ILogger {
    public:
        virtual ~ILogger() = default;
        bool log(); 

    private: 
        motorData get_motor_info(); 
        bool push_to_db(motorData); 

};

#endif
