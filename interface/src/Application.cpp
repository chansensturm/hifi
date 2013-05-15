//
//  Application.cpp
//  interface
//
//  Created by Andrzej Kapolka on 5/10/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.

#include <sstream>

#include <stdlib.h>

#ifdef _WIN32
#include "Syssocket.h"
#include "Systime.h"
#else
#include <sys/time.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#endif

#include <QActionGroup>
#include <QColorDialog>
#include <QDesktopWidget>
#include <QGLWidget>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QShortcut>
#include <QTimer>
#include <QtDebug>
#include <PairingHandler.h>

#include <AgentTypes.h>
#include <PacketHeaders.h>
#include <PerfStat.h>
#include <shared_Log.h>
#include <voxels_Log.h>
#include <avatars_Log.h>

#include "Application.h"
#include "InterfaceConfig.h"
#include "Log.h"
#include "OculusManager.h"
#include "Util.h"
#include "renderer/ProgramObject.h"
#include "ui/TextRenderer.h"

using namespace std;

//  Starfield information
static char STAR_FILE[] = "https://s3-us-west-1.amazonaws.com/highfidelity/stars.txt";
static char STAR_CACHE_FILE[] = "cachedStars.txt";

const glm::vec3 START_LOCATION(6.1f, 0, 1.4f);   //  Where one's own agent begins in the world
                                                 // (will be overwritten if avatar data file is found)

const int IDLE_SIMULATE_MSECS = 16;          //  How often should call simulate and other stuff 
                                             //  in the idle loop?  (60 FPS is default)
                                           
const bool  USING_MOUSE_VIEW_SHIFT = false;
const float MOUSE_VIEW_SHIFT_RATE         = 40.0f;
const float MOUSE_VIEW_SHIFT_YAW_MARGIN   = (float)(1200  * 0.2f);
const float MOUSE_VIEW_SHIFT_PITCH_MARGIN = (float)(800 * 0.2f);
const float MOUSE_VIEW_SHIFT_YAW_LIMIT    = 45.0;
const float MOUSE_VIEW_SHIFT_PITCH_LIMIT  = 30.0;

const bool DISPLAY_HEAD_MOUSE = true;

// customized canvas that simply forwards requests/events to the singleton application
class GLCanvas : public QGLWidget {
protected:
    
    virtual void initializeGL();
    virtual void paintGL();
    virtual void resizeGL(int width, int height);
    
    virtual void keyPressEvent(QKeyEvent* event);
    virtual void keyReleaseEvent(QKeyEvent* event);
    
    virtual void mouseMoveEvent(QMouseEvent* event);
    virtual void mousePressEvent(QMouseEvent* event);
    virtual void mouseReleaseEvent(QMouseEvent* event);
    
    virtual void wheelEvent(QWheelEvent* event);
};

void GLCanvas::initializeGL() {
    static_cast<Application*>(QCoreApplication::instance())->initializeGL();
}

void GLCanvas::paintGL() {
    static_cast<Application*>(QCoreApplication::instance())->paintGL();
}

void GLCanvas::resizeGL(int width, int height) {
    static_cast<Application*>(QCoreApplication::instance())->resizeGL(width, height);
}

void GLCanvas::keyPressEvent(QKeyEvent* event) {
    static_cast<Application*>(QCoreApplication::instance())->keyPressEvent(event);
}

void GLCanvas::keyReleaseEvent(QKeyEvent* event) {
    static_cast<Application*>(QCoreApplication::instance())->keyReleaseEvent(event);
}

void GLCanvas::mouseMoveEvent(QMouseEvent* event) {
    static_cast<Application*>(QCoreApplication::instance())->mouseMoveEvent(event);
}

void GLCanvas::mousePressEvent(QMouseEvent* event) {
    static_cast<Application*>(QCoreApplication::instance())->mousePressEvent(event);
}

void GLCanvas::mouseReleaseEvent(QMouseEvent* event) {
    static_cast<Application*>(QCoreApplication::instance())->mouseReleaseEvent(event);
}

void GLCanvas::wheelEvent(QWheelEvent* event) {
    static_cast<Application*>(QCoreApplication::instance())->wheelEvent(event);
}

Application::Application(int& argc, char** argv) :
        QApplication(argc, argv),
        _window(new QMainWindow(desktop())),
        _glWidget(new GLCanvas()),
        _displayLevels(false),
        _frameCount(0),
        _fps(120.0f),
        _justStarted(true),
        _wantToKillLocalVoxels(false),
        _frustumDrawingMode(FRUSTUM_DRAW_MODE_ALL),
        _viewFrustumOffsetYaw(-135.0),
        _viewFrustumOffsetPitch(0.0),
        _viewFrustumOffsetRoll(0.0),
        _viewFrustumOffsetDistance(25.0),
        _viewFrustumOffsetUp(0.0),
        _mouseViewShiftYaw(0.0f),
        _mouseViewShiftPitch(0.0f),
        _audioScope(256, 200, true),
        _myAvatar(true),
        _mouseX(0),
        _mouseY(0),
        _mousePressed(false),
        _mouseVoxelScale(1.0f / 1024.0f),
        _paintOn(false),
        _dominantColor(0),
        _perfStatsOn(false),
        _chatEntryOn(false),
        _oculusTextureID(0),
        _oculusProgram(0),
        _oculusDistortionScale(1.25),
#ifndef _WIN32
        _audio(&_audioScope),
#endif
        _stopNetworkReceiveThread(false),  
        _packetCount(0),
        _packetsPerSecond(0),
        _bytesPerSecond(0),
        _bytesCount(0)  {  
    
    gettimeofday(&_applicationStartupTime, NULL);
    printLog("Interface Startup:\n");
    
    _voxels.setViewFrustum(&_viewFrustum);
    
    shared_lib::printLog = & ::printLog;
    voxels_lib::printLog = & ::printLog;
    avatars_lib::printLog = & ::printLog;
    
    unsigned int listenPort = AGENT_SOCKET_LISTEN_PORT;
    const char** constArgv = const_cast<const char**>(argv);
    const char* portStr = getCmdOption(argc, constArgv, "--listenPort");
    if (portStr) {
        listenPort = atoi(portStr);
    }
    AgentList::createInstance(AGENT_TYPE_AVATAR, listenPort);
    _enableNetworkThread = !cmdOptionExists(argc, constArgv, "--nonblocking");
    if (!_enableNetworkThread) {
        AgentList::getInstance()->getAgentSocket()->setBlocking(false);
    }
    
    const char* domainIP = getCmdOption(argc, constArgv, "--domain");
    if (domainIP) {
        strcpy(DOMAIN_IP, domainIP);
    }
    
    // Handle Local Domain testing with the --local command line
    if (cmdOptionExists(argc, constArgv, "--local")) {
        printLog("Local Domain MODE!\n");
        int ip = getLocalAddress();
        sprintf(DOMAIN_IP,"%d.%d.%d.%d", (ip & 0xFF), ((ip >> 8) & 0xFF),((ip >> 16) & 0xFF), ((ip >> 24) & 0xFF));
    }
    
    // Check to see if the user passed in a command line option for loading a local
    // Voxel File.
    _voxelsFilename = getCmdOption(argc, constArgv, "-i");
    
    // the callback for our instance of AgentList is attachNewHeadToAgent
    AgentList::getInstance()->linkedDataCreateCallback = &attachNewHeadToAgent;
    
    #ifdef _WIN32
    WSADATA WsaData;
    int wsaresult = WSAStartup(MAKEWORD(2,2), &WsaData);
    #endif

    // start the agentList threads
    AgentList::getInstance()->startSilentAgentRemovalThread();
    AgentList::getInstance()->startDomainServerCheckInThread();
    AgentList::getInstance()->startPingUnknownAgentsThread();
    
    _window->setCentralWidget(_glWidget);
    
    initMenu();
    
    QRect available = desktop()->availableGeometry();
    _window->resize(available.size());
    _window->setVisible(true);
    _glWidget->setFocusPolicy(Qt::StrongFocus);
    _glWidget->setFocus();
    
    // enable mouse tracking; otherwise, we only get drag events
    _glWidget->setMouseTracking(true);
    
    // initialization continues in initializeGL when OpenGL context is ready
}

void Application::initializeGL() {
    printLog( "Created Display Window.\n" );
    
    int argc = 0;
    glutInit(&argc, 0);
    
    #ifdef _WIN32
    glewInit();
    printLog( "Glew Init complete.\n" );
    #endif
    
    // Before we render anything, let's set up our viewFrustumOffsetCamera with a sufficiently large
    // field of view and near and far clip to make it interesting.
    //viewFrustumOffsetCamera.setFieldOfView(90.0);
    _viewFrustumOffsetCamera.setNearClip(0.1);
    _viewFrustumOffsetCamera.setFarClip(500.0 * TREE_SCALE);
    
    initDisplay();
    printLog( "Initialized Display.\n" );
    
    init();
    printLog( "Init() complete.\n" );
    
    // Check to see if the user passed in a command line option for randomizing colors
    bool wantColorRandomizer = !arguments().contains("--NoColorRandomizer");
    
    // Check to see if the user passed in a command line option for loading a local
    // Voxel File. If so, load it now.
    if (!_voxelsFilename.isEmpty()) {
        _voxels.loadVoxelsFile(_voxelsFilename.constData(), wantColorRandomizer);
        printLog("Local Voxel File loaded.\n");
    }
    
    // create thread for receipt of data via UDP
    if (_enableNetworkThread) {
        pthread_create(&_networkReceiveThread, NULL, networkReceive, NULL);
        printLog("Network receive thread created.\n"); 
    }
    
    _myAvatar.readAvatarDataFromFile();
    
    // call terminate before exiting
    connect(this, SIGNAL(aboutToQuit()), SLOT(terminate()));
    
    // call our timer function every second
    QTimer* timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), SLOT(timer()));
    timer->start(1000);
    
    // call our idle function whenever we can
    QTimer* idleTimer = new QTimer(this);
    connect(idleTimer, SIGNAL(timeout()), SLOT(idle()));
    idleTimer->start(0);
}

void Application::paintGL() {
    PerfStat("display");

    glEnable(GL_LINE_SMOOTH);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    
    glPushMatrix();  {
        glLoadIdentity();
    
        // camera settings
        if (OculusManager::isConnected()) {
            _myAvatar.setDisplayingHead(false);            
            _myCamera.setUpShift       (0.0f);
            _myCamera.setDistance      (0.0f);
            _myCamera.setTightness     (100.0f); 
            _myCamera.setTargetPosition(_myAvatar.getHeadPosition());
            _myCamera.setTargetRotation(_myAvatar.getBodyYaw() + _myAvatar.getHeadYaw(),
                                        -_myAvatar.getHeadPitch(), _myAvatar.getHeadRoll());
         
        } else if (_myCamera.getMode() == CAMERA_MODE_MIRROR) {
            _myCamera.setTargetPosition(_myAvatar.getSpringyHeadPosition());
            _myCamera.setTargetRotation(_myAvatar.getBodyYaw() - 180.0f, 0.0f, 0.0f); 
        
        } else {
            if (_myCamera.getMode() == CAMERA_MODE_FIRST_PERSON) {
                _myCamera.setTargetPosition(_myAvatar.getSpringyHeadPosition());
                _myCamera.setTargetRotation(_myAvatar.getAbsoluteHeadYaw() - _mouseViewShiftYaw,
                                            _myAvatar.getRenderPitch() + _mouseViewShiftPitch, 0.0f);                
            
            } else if (_myCamera.getMode() == CAMERA_MODE_THIRD_PERSON) {
                _myCamera.setTargetPosition(_myAvatar.getHeadPosition());
                _myCamera.setTargetRotation(_myAvatar.getBodyYaw() - _mouseViewShiftYaw, _mouseViewShiftPitch, 0.0f);
            }  
        }
                
        // important...
        _myCamera.update( 1.f/_fps );
        
        // Render anything (like HUD items) that we want to be in 3D but not in worldspace
        /*
        const float HUD_Z_OFFSET = -5.f;
        glPushMatrix();
        glm::vec3 test(0.5, 0.5, 0.5);
        glTranslatef(1, 1, HUD_Z_OFFSET);
        drawVector(&test);
        glPopMatrix();
         */
        
        
        // Note: whichCamera is used to pick between the normal camera myCamera for our 
        // main camera, vs, an alternate camera. The alternate camera we support right now
        // is the viewFrustumOffsetCamera. But theoretically, we could use this same mechanism
        // to add other cameras.
        //
        // Why have two cameras? Well, one reason is that because in the case of the renderViewFrustum()
        // code, we want to keep the state of "myCamera" intact, so we can render what the view frustum of
        // myCamera is. But we also want to do meaningful camera transforms on OpenGL for the offset camera
        Camera whichCamera = _myCamera;

        if (_viewFrustumFromOffset->isChecked() && _frustumOn->isChecked()) {

            // set the camera to third-person view but offset so we can see the frustum
            _viewFrustumOffsetCamera.setTargetYaw(_viewFrustumOffsetYaw + _myAvatar.getBodyYaw());
            _viewFrustumOffsetCamera.setPitch    (_viewFrustumOffsetPitch   );
            _viewFrustumOffsetCamera.setRoll     (_viewFrustumOffsetRoll    ); 
            _viewFrustumOffsetCamera.setUpShift  (_viewFrustumOffsetUp      );
            _viewFrustumOffsetCamera.setDistance (_viewFrustumOffsetDistance);
            _viewFrustumOffsetCamera.update(1.f/_fps);
            whichCamera = _viewFrustumOffsetCamera;
        }        

        // transform view according to whichCamera
        // could be myCamera (if in normal mode)
        // or could be viewFrustumOffsetCamera if in offset mode
        // I changed the ordering here - roll is FIRST (JJV) 

        glRotatef   (        whichCamera.getRoll(),  IDENTITY_FRONT.x, IDENTITY_FRONT.y, IDENTITY_FRONT.z);
        glRotatef   (        whichCamera.getPitch(), IDENTITY_RIGHT.x, IDENTITY_RIGHT.y, IDENTITY_RIGHT.z);
        glRotatef   (180.0 - whichCamera.getYaw(),     IDENTITY_UP.x,    IDENTITY_UP.y,    IDENTITY_UP.z   );

        glTranslatef(-whichCamera.getPosition().x, -whichCamera.getPosition().y, -whichCamera.getPosition().z);
        
        //  Setup 3D lights (after the camera transform, so that they are positioned in world space)
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        
        glm::vec3 relativeSunLoc = glm::normalize(_environment.getSunLocation() - whichCamera.getPosition());
        GLfloat light_position0[] = { relativeSunLoc.x, relativeSunLoc.y, relativeSunLoc.z, 0.0 };
        glLightfv(GL_LIGHT0, GL_POSITION, light_position0);
        GLfloat ambient_color[] = { 0.7, 0.7, 0.8 };   
        glLightfv(GL_LIGHT0, GL_AMBIENT, ambient_color);
        GLfloat diffuse_color[] = { 0.8, 0.7, 0.7 };  
        glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse_color);
        GLfloat specular_color[] = { 1.0, 1.0, 1.0, 1.0};
        glLightfv(GL_LIGHT0, GL_SPECULAR, specular_color);
        
        glMaterialfv(GL_FRONT, GL_SPECULAR, specular_color);
        glMateriali(GL_FRONT, GL_SHININESS, 96);
        
        if (_oculusOn->isChecked()) {
            displayOculus(whichCamera);
            
        } else {
            displaySide(whichCamera);
            glPopMatrix();
            
            displayOverlay();
        }
    }
    
    _frameCount++;
    
    //  If application has just started, report time from startup to now (first frame display)
    if (_justStarted) {
        float startupTime = (usecTimestampNow() - usecTimestamp(&_applicationStartupTime))/1000000.0;
        _justStarted = false;
        char title[30];
        snprintf(title, 30, "Interface: %4.2f seconds", startupTime);
        _window->setWindowTitle(title);
    }
}

void Application::resizeGL(int width, int height) {
    float aspectRatio = ((float)width/(float)height); // based on screen resize

    // get the lens details from the current camera
    Camera& camera = _viewFrustumFromOffset->isChecked() ? _viewFrustumOffsetCamera : _myCamera;
    float nearClip = camera.getNearClip();
    float farClip = camera.getFarClip();
    float fov;

    if (_oculusOn->isChecked()) {
        // more magic numbers; see Oculus SDK docs, p. 32
        camera.setAspectRatio(aspectRatio *= 0.5);
        camera.setFieldOfView(fov = 2 * atan((0.0468 * _oculusDistortionScale) / 0.041) * (180 / PI));
        
        // resize the render texture
        if (_oculusTextureID != 0) {
            glBindTexture(GL_TEXTURE_2D, _oculusTextureID);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    } else {
        camera.setAspectRatio(aspectRatio);
        camera.setFieldOfView(fov = 60);
    }

    // Tell our viewFrustum about this change
    _viewFrustum.setAspectRatio(aspectRatio);

    glViewport(0, 0, width, height); // shouldn't this account for the menu???

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    
    // XXXBHG - If we're in view frustum mode, then we need to do this little bit of hackery so that
    // OpenGL won't clip our frustum rendering lines. This is a debug hack for sure! Basically, this makes
    // the near clip a little bit closer (therefor you see more) and the far clip a little bit farther (also,
    // to see more.)
    if (_frustumOn->isChecked()) {
        nearClip -= 0.01f;
        farClip  += 0.01f;
    }
    
    // On window reshape, we need to tell OpenGL about our new setting
    gluPerspective(fov,aspectRatio,nearClip,farClip);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void sendVoxelServerEraseAll() {
    char message[100];
    sprintf(message,"%c%s",'Z',"erase all");
    int messageSize = strlen(message) + 1;
    AgentList::getInstance()->broadcastToAgents((unsigned char*) message, messageSize, &AGENT_TYPE_VOXEL, 1);
}

static void sendVoxelServerAddScene() {
    char message[100];
    sprintf(message,"%c%s",'Z',"add scene");
    int messageSize = strlen(message) + 1;
    AgentList::getInstance()->broadcastToAgents((unsigned char*)message, messageSize, &AGENT_TYPE_VOXEL, 1);
}

void Application::keyPressEvent(QKeyEvent* event) {
    if (_chatEntryOn) {
        if (_chatEntry.keyPressEvent(event)) {
            _myAvatar.setKeyState(event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete ?
                DELETE_KEY_DOWN : INSERT_KEY_DOWN);            
            _myAvatar.setChatMessage(string(_chatEntry.getContents().size(), SOLID_BLOCK_CHAR));
            
        } else {
            _myAvatar.setChatMessage(_chatEntry.getContents());
            _chatEntry.clear();
            _chatEntryOn = false;
            setMenuShortcutsEnabled(true);
        }
        return;
    }
    
    bool shifted = event->modifiers().testFlag(Qt::ShiftModifier);
    switch (event->key()) {
        case Qt::Key_BracketLeft:
            _viewFrustumOffsetYaw -= 0.5;
            break;
            
        case Qt::Key_BracketRight:
            _viewFrustumOffsetYaw += 0.5;
            break;
        
        case Qt::Key_BraceLeft:
            _viewFrustumOffsetPitch -= 0.5;
            break;
        
        case Qt::Key_BraceRight:
            _viewFrustumOffsetPitch += 0.5;
            break;
            
        case Qt::Key_ParenLeft:
            _viewFrustumOffsetRoll -= 0.5;
            break;
            
        case Qt::Key_ParenRight:
            _viewFrustumOffsetRoll += 0.5;
            break;
            
        case Qt::Key_Less:
            _viewFrustumOffsetDistance -= 0.5;
            break;
            
        case Qt::Key_Greater:
            _viewFrustumOffsetDistance += 0.5;
            break;
            
        case Qt::Key_Comma:
            _viewFrustumOffsetUp -= 0.05;
            break;
        
        case Qt::Key_Period:
            _viewFrustumOffsetUp += 0.05;
            break;
        
        case Qt::Key_Ampersand:
            _paintOn = !_paintOn;
            setupPaintingVoxel();
            break;
    
        case Qt::Key_AsciiCircum:
            shiftPaintingColor();
            break;
    
        case Qt::Key_Minus:
            sendVoxelServerEraseAll();
            break;
        
        case Qt::Key_Percent:
            sendVoxelServerAddScene();
            break;
        
        case Qt::Key_L:
            _displayLevels = !_displayLevels;
            break;
        
        case Qt::Key_E:
            _myAvatar.setDriveKeys(UP, 1);
            break;
            
        case Qt::Key_C:
            _myAvatar.setDriveKeys(DOWN, 1);
            break;
            
        case Qt::Key_W:
            _myAvatar.setDriveKeys(FWD, 1);
            break;
            
        case Qt::Key_S:
            _myAvatar.setDriveKeys(BACK, 1);
            break;
            
        case Qt::Key_Space:
            resetSensors();
            break;
        
        case Qt::Key_A:
            _myAvatar.setDriveKeys(ROT_LEFT, 1);
            break;
            
        case Qt::Key_D:
            _myAvatar.setDriveKeys(ROT_RIGHT, 1);
            break;
        
        case Qt::Key_Return:
        case Qt::Key_Enter:
            _chatEntryOn = true;
            _myAvatar.setKeyState(NO_KEY_DOWN);
            _myAvatar.setChatMessage(string());
            setMenuShortcutsEnabled(false);
            break;
            
        case Qt::Key_Up:
            _myAvatar.setDriveKeys(shifted ? UP : FWD, 1);
            break;
            
        case Qt::Key_Down:
            _myAvatar.setDriveKeys(shifted ? DOWN : BACK, 1);
            break;
            
        case Qt::Key_Left:
            _myAvatar.setDriveKeys(shifted ? LEFT : ROT_LEFT, 1);
            break;
            
        case Qt::Key_Right:
            _myAvatar.setDriveKeys(shifted ? RIGHT : ROT_RIGHT, 1);
            break;
        
        default:
            event->ignore();
            break;
    }
}

void Application::keyReleaseEvent(QKeyEvent* event) {
    if (_chatEntryOn) {
        _myAvatar.setKeyState(NO_KEY_DOWN);
        return;
    }

    switch (event->key()) {
        case Qt::Key_E:
            _myAvatar.setDriveKeys(UP, 0);
            break;
        
        case Qt::Key_C:
            _myAvatar.setDriveKeys(DOWN, 0);
            break;
        
        case Qt::Key_W:
            _myAvatar.setDriveKeys(FWD, 0);
            break;
            
        case Qt::Key_S:
            _myAvatar.setDriveKeys(BACK, 0);
            break;
            
        case Qt::Key_A:
            _myAvatar.setDriveKeys(ROT_LEFT, 0);
            break;
                
        case Qt::Key_D:
            _myAvatar.setDriveKeys(ROT_RIGHT, 0);
            break;
            
        case Qt::Key_Up:
            _myAvatar.setDriveKeys(FWD, 0);
            _myAvatar.setDriveKeys(UP, 0);
            break;
            
        case Qt::Key_Down:
            _myAvatar.setDriveKeys(BACK, 0);
            _myAvatar.setDriveKeys(DOWN, 0);
            break;
            
        case Qt::Key_Left:
            _myAvatar.setDriveKeys(LEFT, 0);
            _myAvatar.setDriveKeys(ROT_LEFT, 0);
            break;
            
        case Qt::Key_Right:
            _myAvatar.setDriveKeys(RIGHT, 0);
            _myAvatar.setDriveKeys(ROT_RIGHT, 0);
            break;
    
        default:
            event->ignore();
            break;
    }
}

void Application::mouseMoveEvent(QMouseEvent* event) {
    _mouseX = event->x();
    _mouseY = event->y();
    
    // detect drag
    glm::vec3 mouseVoxelPos(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z);
    if (_colorVoxelMode->isChecked() && event->buttons().testFlag(Qt::LeftButton) && mouseVoxelPos != _lastMouseVoxelPos) {
        addVoxelUnderCursor();
    }
}

void Application::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        _mouseX = event->x();
        _mouseY = event->y();
        _mousePressed = true;
       
        if (_addVoxelMode->isChecked() || _colorVoxelMode->isChecked()) {
            addVoxelUnderCursor();
        
        } else if (_deleteVoxelMode->isChecked()) {
            deleteVoxelUnderCursor();    
        }
    } else if (event->button() == Qt::RightButton && checkedVoxelModeAction() != 0) {
        deleteVoxelUnderCursor();
    }
}

void Application::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        _mouseX = event->x();
        _mouseY = event->y();
        _mousePressed = false;
    }
}

void Application::wheelEvent(QWheelEvent* event) {
    if (checkedVoxelModeAction() == 0) {
        event->ignore();
        return;
    }
    if (event->delta() > 0) {
        increaseVoxelSize();
    } else {
        decreaseVoxelSize();
    }
}
    
//  Every second, check the frame rates and other stuff
void Application::timer() {
    gettimeofday(&_timerEnd, NULL);
    _fps = (float)_frameCount / ((float)diffclock(&_timerStart, &_timerEnd) / 1000.f);
    _packetsPerSecond = (float)_packetCount / ((float)diffclock(&_timerStart, &_timerEnd) / 1000.f);
    _bytesPerSecond = (float)_bytesCount / ((float)diffclock(&_timerStart, &_timerEnd) / 1000.f);
    _frameCount = 0;
    _packetCount = 0;
    _bytesCount = 0;
    
    gettimeofday(&_timerStart, NULL);
    
    // if we haven't detected gyros, check for them now
    if (!_serialPort.active) {
        _serialPort.pair();
    }
}

static glm::vec3 getFaceVector(BoxFace face) {
    switch (face) {
        case MIN_X_FACE:
            return glm::vec3(-1, 0, 0);
        
        case MAX_X_FACE:
            return glm::vec3(1, 0, 0);
        
        case MIN_Y_FACE:
            return glm::vec3(0, -1, 0);
        
        case MAX_Y_FACE:
            return glm::vec3(0, 1, 0);
        
        case MIN_Z_FACE:
            return glm::vec3(0, 0, -1);
            
        case MAX_Z_FACE:
            return glm::vec3(0, 0, 1);
    }
}

//Find and return the gravity vector at this location
static glm::vec3 getGravity(const glm::vec3& pos) {
    //
    //  For now, we'll test this with a simple global lookup, but soon we will add getting this
    //  from the domain/voxelserver (or something similar)
    //
    if ((pos.x >  0.f) &&
        (pos.x < 10.f) &&
        (pos.z >  0.f) &&
        (pos.z < 10.f) &&
        (pos.y >  0.f) &&
        (pos.y <  3.f)) {
        //  If above ground plane, turn gravity on
        return glm::vec3(0.f, -1.f, 0.f);
    } else {
        //  If flying in space, turn gravity OFF
        return glm::vec3(0.f, 0.f, 0.f);
    }
}

void Application::idle() {
    timeval check;
    gettimeofday(&check, NULL);
    
    //  Only run simulation code if more than IDLE_SIMULATE_MSECS have passed since last time
    
    if (diffclock(&_lastTimeIdle, &check) > IDLE_SIMULATE_MSECS) {
        
        float deltaTime = 1.f/_fps;

        // update behaviors for avatar hand movement: handControl takes mouse values as input, 
        // and gives back 3D values modulated for smooth transitioning between interaction modes.
        _handControl.update(_mouseX, _mouseY);
        _myAvatar.setHandMovementValues(_handControl.getValues());        
        
        // tell my avatar if the mouse is being pressed...
        _myAvatar.setMousePressed(_mousePressed);
        
        // check what's under the mouse and update the mouse voxel
        glm::vec3 mouseRayOrigin, mouseRayDirection;
        _viewFrustum.computePickRay(_mouseX / (float)_glWidget->width(),
            _mouseY / (float)_glWidget->height(), mouseRayOrigin, mouseRayDirection);

        // tell my avatar the posiion and direction of the ray projected ino the world based on the mouse position        
        _myAvatar.setMouseRay(mouseRayOrigin, mouseRayDirection);

        _mouseVoxel.s = 0.0f;
        if (checkedVoxelModeAction() != 0) {
            float distance;
            BoxFace face;
            if (_voxels.findRayIntersection(mouseRayOrigin, mouseRayDirection, _mouseVoxel, distance, face)) {            
                // find the nearest voxel with the desired scale
                if (_mouseVoxelScale > _mouseVoxel.s) {
                    // choose the larger voxel that encompasses the one selected
                    _mouseVoxel.x = _mouseVoxelScale * floorf(_mouseVoxel.x / _mouseVoxelScale); 
                    _mouseVoxel.y = _mouseVoxelScale * floorf(_mouseVoxel.y / _mouseVoxelScale); 
                    _mouseVoxel.z = _mouseVoxelScale * floorf(_mouseVoxel.z / _mouseVoxelScale);
                    _mouseVoxel.s = _mouseVoxelScale;
                
                } else {
                    glm::vec3 faceVector = getFaceVector(face);
                    if (_mouseVoxelScale < _mouseVoxel.s) {
                        // find the closest contained voxel
                        glm::vec3 pt = (mouseRayOrigin + mouseRayDirection * distance) / (float)TREE_SCALE -
                            faceVector * (_mouseVoxelScale * 0.5f);
                        _mouseVoxel.x = _mouseVoxelScale * floorf(pt.x / _mouseVoxelScale); 
                        _mouseVoxel.y = _mouseVoxelScale * floorf(pt.y / _mouseVoxelScale); 
                        _mouseVoxel.z = _mouseVoxelScale * floorf(pt.z / _mouseVoxelScale);
                        _mouseVoxel.s = _mouseVoxelScale;
                    }
                    if (_addVoxelMode->isChecked()) {
                        // use the face to determine the side on which to create a neighbor
                        _mouseVoxel.x += faceVector.x * _mouseVoxel.s;
                        _mouseVoxel.y += faceVector.y * _mouseVoxel.s;
                        _mouseVoxel.z += faceVector.z * _mouseVoxel.s;
                    }
                }
            } else if (_addVoxelMode->isChecked()) {
                // place the voxel a fixed distance away
                float worldMouseVoxelScale = _mouseVoxelScale * TREE_SCALE;
                glm::vec3 pt = mouseRayOrigin + mouseRayDirection * (2.0f + worldMouseVoxelScale * 0.5f);
                _mouseVoxel.x = _mouseVoxelScale * floorf(pt.x / worldMouseVoxelScale);
                _mouseVoxel.y = _mouseVoxelScale * floorf(pt.y / worldMouseVoxelScale);
                _mouseVoxel.z = _mouseVoxelScale * floorf(pt.z / worldMouseVoxelScale);
                _mouseVoxel.s = _mouseVoxelScale;
            }
            
            if (_deleteVoxelMode->isChecked()) {
                // red indicates deletion
                _mouseVoxel.red = 255;
                _mouseVoxel.green = _mouseVoxel.blue = 0;
            
            } else { // _addVoxelMode->isChecked() || _colorVoxelMode->isChecked()
                QColor paintColor = _voxelPaintColor->data().value<QColor>();
                _mouseVoxel.red = paintColor.red();
                _mouseVoxel.green = paintColor.green();
                _mouseVoxel.blue = paintColor.blue();
            }
        }
        
        // walking triggers the handControl to stop
        if (_myAvatar.getMode() == AVATAR_MODE_WALKING) {
            _handControl.stop();
            _mouseViewShiftYaw   *= 0.9;
            _mouseViewShiftPitch *= 0.9;
        }
        
        //  Read serial port interface devices
        if (_serialPort.active) {
            _serialPort.readData();
        }
        
        //  Sample hardware, update view frustum if needed, and send avatar data to mixer/agents
        updateAvatar(deltaTime);

        // read incoming packets from network
        if (!_enableNetworkThread) {
            networkReceive(0);
        }
        
        //loop through all the remote avatars and simulate them...
        AgentList* agentList = AgentList::getInstance();
        agentList->lock();
        for(AgentList::iterator agent = agentList->begin(); agent != agentList->end(); agent++) {
            if (agent->getLinkedData() != NULL) {
                Avatar *avatar = (Avatar *)agent->getLinkedData();
                avatar->simulate(deltaTime);
                avatar->setMouseRay(mouseRayOrigin, mouseRayDirection);
            }
        }
        agentList->unlock();
    
        _myAvatar.setGravity(getGravity(_myAvatar.getPosition()));
        _myAvatar.simulate(deltaTime);
        
        //  Update audio stats for procedural sounds
        #ifndef _WIN32
        _audio.setLastAcceleration(_myAvatar.getThrust());
        _audio.setLastVelocity(_myAvatar.getVelocity());
        #endif
        
        _glWidget->updateGL();
        _lastTimeIdle = check;
    }
}

void Application::terminate() {
    // Close serial port
    // close(serial_fd);
    
    _myAvatar.writeAvatarDataToFile();

    if (_enableNetworkThread) {
        _stopNetworkReceiveThread = true;
        pthread_join(_networkReceiveThread, NULL); 
    }
}

void Application::pair() {
    PairingHandler::sendPairRequest();
}

void Application::setHead(bool head) {
    #ifndef _WIN32
    _audio.setMixerLoopbackFlag(head);
    #endif
    
    if (head) {
        Camera::CameraFollowingAttributes a;
        a.upShift   = 0.0f;
        a.distance  = 0.2f;
        a.tightness = 100.0f;
        _myCamera.setMode(CAMERA_MODE_MIRROR, a);
        _myAvatar.setDisplayingHead(true);  
    } else {
        Camera::CameraFollowingAttributes a;
        a.upShift   = -0.2f;
        a.distance  = 1.5f;
        a.tightness = 8.0f;
        _myCamera.setMode(CAMERA_MODE_THIRD_PERSON, a);
        _myAvatar.setDisplayingHead(true);  
    } 
}

void Application::setNoise(bool noise) {
    _myAvatar.setNoise(noise);
}

void Application::setFullscreen(bool fullscreen) {
    _window->setWindowState(fullscreen ? (_window->windowState() | Qt::WindowFullScreen) :
        (_window->windowState() & ~Qt::WindowFullScreen));
}

void Application::setRenderFirstPerson(bool firstPerson) {
    if (firstPerson) {
        Camera::CameraFollowingAttributes a;
        a.upShift   = 0.0f;
        a.distance  = 0.0f;
        a.tightness = 100.0f;
        _myCamera.setMode(CAMERA_MODE_FIRST_PERSON, a);
        _myAvatar.setDisplayingHead(false);  
    
    } else {
        Camera::CameraFollowingAttributes a;            
        a.upShift   = -0.2f;
        a.distance  = 1.5f;
        a.tightness = 8.0f;
        _myCamera.setMode(CAMERA_MODE_THIRD_PERSON, a);
        _myAvatar.setDisplayingHead(true);  
    } 
}

void Application::setOculus(bool oculus) {
    resizeGL(_glWidget->width(), _glWidget->height());
}

void Application::setFrustumOffset(bool frustumOffset) {
    // reshape so that OpenGL will get the right lens details for the camera of choice  
    resizeGL(_glWidget->width(), _glWidget->height());
}

void Application::cycleFrustumRenderMode() {
    _frustumDrawingMode = (FrustumDrawMode)((_frustumDrawingMode + 1) % FRUSTUM_DRAW_MODE_COUNT);
    updateFrustumRenderModeAction();
}

void Application::setRenderWarnings(bool renderWarnings) {
    _voxels.setRenderPipelineWarnings(renderWarnings);
}

void Application::doKillLocalVoxels() {
    _wantToKillLocalVoxels = true;
}

void Application::doRandomizeVoxelColors() {
    _voxels.randomizeVoxelColors();
}

void Application::doFalseRandomizeVoxelColors() {
    _voxels.falseColorizeRandom();
}

void Application::doFalseRandomizeEveryOtherVoxelColors() {
    _voxels.falseColorizeRandomEveryOther();
}

void Application::doFalseColorizeByDistance() {
    loadViewFrustum(_viewFrustum);
    _voxels.falseColorizeDistanceFromView(&_viewFrustum);
}

void Application::doFalseColorizeInView() {
    loadViewFrustum(_viewFrustum);
    // we probably want to make sure the viewFrustum is initialized first
    _voxels.falseColorizeInView(&_viewFrustum);
}

void Application::doTrueVoxelColors() {
    _voxels.trueColorize();
}

void Application::doTreeStats() {
    _voxels.collectStatsForTreesAndVBOs();
}

void Application::setWantsMonochrome(bool wantsMonochrome) {
    _myAvatar.setWantColor(!wantsMonochrome);
}

void Application::setWantsResIn(bool wantsResIn) {
    _myAvatar.setWantResIn(wantsResIn);
}


void Application::setWantsDelta(bool wantsDelta) {
    _myAvatar.setWantDelta(wantsDelta);
}

void Application::updateVoxelModeActions() {
    // only the sender can be checked
    foreach (QAction* action, _voxelModeActions->actions()) {
        if (action->isChecked() && action != sender()) {
            action->setChecked(false);
        }
    } 
}

static void sendVoxelEditMessage(PACKET_HEADER header, VoxelDetail& detail) {
    unsigned char* bufferOut;
    int sizeOut;

    if (createVoxelEditMessage(header, 0, 1, &detail, bufferOut, sizeOut)){
        AgentList::getInstance()->broadcastToAgents(bufferOut, sizeOut, &AGENT_TYPE_VOXEL, 1);
        delete bufferOut;
    }
}

void Application::addVoxelInFrontOfAvatar() {
    VoxelDetail detail;
    
    glm::vec3 position = (_myAvatar.getPosition() + _myAvatar.getCameraDirection()) * (1.0f / TREE_SCALE);
    detail.s = _mouseVoxelScale;
    
    detail.x = detail.s * floor(position.x / detail.s);
    detail.y = detail.s * floor(position.y / detail.s);
    detail.z = detail.s * floor(position.z / detail.s);
    QColor paintColor = _voxelPaintColor->data().value<QColor>();
    detail.red = paintColor.red();
    detail.green = paintColor.green();
    detail.blue = paintColor.blue();
    
    PACKET_HEADER message = (_destructiveAddVoxel->isChecked() ?
        PACKET_HEADER_SET_VOXEL_DESTRUCTIVE : PACKET_HEADER_SET_VOXEL);
    sendVoxelEditMessage(message, detail);
    
    // create the voxel locally so it appears immediately            
    _voxels.createVoxel(detail.x, detail.y, detail.z, detail.s,
        detail.red, detail.green, detail.blue, _destructiveAddVoxel->isChecked());
}

void Application::decreaseVoxelSize() {
    _mouseVoxelScale /= 2;
}

void Application::increaseVoxelSize() {
    _mouseVoxelScale *= 2;
}
    
static QIcon createSwatchIcon(const QColor& color) {
    QPixmap map(16, 16);
    map.fill(color);
    return QIcon(map);
}

void Application::chooseVoxelPaintColor() {
    QColor selected = QColorDialog::getColor(_voxelPaintColor->data().value<QColor>(), _glWidget, "Voxel Paint Color");
    if (selected.isValid()) {
        _voxelPaintColor->setData(selected);
        _voxelPaintColor->setIcon(createSwatchIcon(selected));
    }
    
    // restore the main window's active state
    _window->activateWindow();
}
    
void Application::initMenu() {
    QMenuBar* menuBar = new QMenuBar();
    _window->setMenuBar(menuBar);
    
    QMenu* fileMenu = menuBar->addMenu("File");
    fileMenu->addAction("Pair", this, SLOT(pair()));
    fileMenu->addAction("Quit", this, SLOT(quit()), Qt::Key_Q);
    
    QMenu* optionsMenu = menuBar->addMenu("Options");
    (_lookingInMirror = optionsMenu->addAction("Mirror", this, SLOT(setHead(bool)), Qt::Key_H))->setCheckable(true);
    optionsMenu->addAction("Noise", this, SLOT(setNoise(bool)), Qt::Key_N)->setCheckable(true);
    (_gyroLook = optionsMenu->addAction("Gyro Look"))->setCheckable(true);
    _gyroLook->setChecked(true);
    optionsMenu->addAction("Fullscreen", this, SLOT(setFullscreen(bool)), Qt::Key_F)->setCheckable(true);
    
    QMenu* renderMenu = menuBar->addMenu("Render");
    (_renderVoxels = renderMenu->addAction("Voxels"))->setCheckable(true);
    _renderVoxels->setChecked(true);
    _renderVoxels->setShortcut(Qt::Key_V);
    (_renderStarsOn = renderMenu->addAction("Stars"))->setCheckable(true);
    _renderStarsOn->setChecked(true);
    _renderStarsOn->setShortcut(Qt::Key_Asterisk);
    (_renderAtmosphereOn = renderMenu->addAction("Atmosphere"))->setCheckable(true);
    _renderAtmosphereOn->setChecked(true);
    _renderAtmosphereOn->setShortcut(Qt::SHIFT | Qt::Key_A);
    (_renderAvatarsOn = renderMenu->addAction("Avatars"))->setCheckable(true);
    _renderAvatarsOn->setChecked(true);
    renderMenu->addAction("First Person", this, SLOT(setRenderFirstPerson(bool)), Qt::Key_P)->setCheckable(true);
    (_oculusOn = renderMenu->addAction("Oculus", this, SLOT(setOculus(bool)), Qt::Key_O))->setCheckable(true);
    
    QMenu* toolsMenu = menuBar->addMenu("Tools");
    (_renderStatsOn = toolsMenu->addAction("Stats"))->setCheckable(true);
    _renderStatsOn->setShortcut(Qt::Key_Slash);
    (_logOn = toolsMenu->addAction("Log"))->setCheckable(true);
    _logOn->setChecked(true);
    
    QMenu* voxelMenu = menuBar->addMenu("Voxels");
    _voxelModeActions = new QActionGroup(this);
    _voxelModeActions->setExclusive(false); // exclusivity implies one is always checked
    (_addVoxelMode = voxelMenu->addAction(
        "Add Voxel Mode", this, SLOT(updateVoxelModeActions()), Qt::Key_1))->setCheckable(true);
    _voxelModeActions->addAction(_addVoxelMode);
    (_deleteVoxelMode = voxelMenu->addAction(
        "Delete Voxel Mode", this, SLOT(updateVoxelModeActions()), Qt::Key_2))->setCheckable(true);
    _voxelModeActions->addAction(_deleteVoxelMode);
    (_colorVoxelMode = voxelMenu->addAction(
        "Color Voxel Mode", this, SLOT(updateVoxelModeActions()), Qt::Key_3))->setCheckable(true);
    _voxelModeActions->addAction(_colorVoxelMode);
    
    voxelMenu->addAction("Place Voxel", this, SLOT(addVoxelInFrontOfAvatar()), Qt::Key_4);
    voxelMenu->addAction("Decrease Voxel Size", this, SLOT(decreaseVoxelSize()), Qt::Key_5);
    voxelMenu->addAction("Increase Voxel Size", this, SLOT(increaseVoxelSize()), Qt::Key_6);
    
    _voxelPaintColor = voxelMenu->addAction("Voxel Paint Color", this, SLOT(chooseVoxelPaintColor()), Qt::Key_7);
    QColor paintColor(128, 128, 128);
    _voxelPaintColor->setData(paintColor);
    _voxelPaintColor->setIcon(createSwatchIcon(paintColor));
    (_destructiveAddVoxel = voxelMenu->addAction("Create Voxel is Destructive"))->setCheckable(true);
    
    QMenu* frustumMenu = menuBar->addMenu("Frustum");
    (_frustumOn = frustumMenu->addAction("Display Frustum"))->setCheckable(true); 
    _frustumOn->setShortcut(Qt::SHIFT | Qt::Key_F);
    (_viewFrustumFromOffset = frustumMenu->addAction(
        "Use Offset Camera", this, SLOT(setFrustumOffset(bool)), Qt::SHIFT | Qt::Key_O))->setCheckable(true); 
    (_cameraFrustum = frustumMenu->addAction("Switch Camera"))->setCheckable(true);
    _cameraFrustum->setChecked(true);
    _cameraFrustum->setShortcut(Qt::SHIFT | Qt::Key_C);
    _frustumRenderModeAction = frustumMenu->addAction(
        "Render Mode", this, SLOT(cycleFrustumRenderMode()), Qt::SHIFT | Qt::Key_R); 
    updateFrustumRenderModeAction();
    
    QMenu* debugMenu = menuBar->addMenu("Debug");
    debugMenu->addAction("Show Render Pipeline Warnings", this, SLOT(setRenderWarnings(bool)))->setCheckable(true);
    debugMenu->addAction("Kill Local Voxels", this, SLOT(doKillLocalVoxels()));
    debugMenu->addAction("Randomize Voxel TRUE Colors", this, SLOT(doRandomizeVoxelColors()));
    debugMenu->addAction("FALSE Color Voxels Randomly", this, SLOT(doFalseRandomizeVoxelColors()));
    debugMenu->addAction("FALSE Color Voxel Every Other Randomly", this, SLOT(doFalseRandomizeEveryOtherVoxelColors()));
    debugMenu->addAction("FALSE Color Voxels by Distance", this, SLOT(doFalseColorizeByDistance()));
    debugMenu->addAction("FALSE Color Voxel Out of View", this, SLOT(doFalseColorizeInView()));
    debugMenu->addAction("Show TRUE Colors", this, SLOT(doTrueVoxelColors()));
    debugMenu->addAction("Calculate Tree Stats", this, SLOT(doTreeStats()), Qt::SHIFT | Qt::Key_S);
    debugMenu->addAction("Wants Res-In", this, SLOT(setWantsResIn(bool)))->setCheckable(true);
    debugMenu->addAction("Wants Monochrome", this, SLOT(setWantsMonochrome(bool)))->setCheckable(true);
    debugMenu->addAction("Wants View Delta Sending", this, SLOT(setWantsDelta(bool)))->setCheckable(true);
}

void Application::updateFrustumRenderModeAction() {
    switch (_frustumDrawingMode) {
        default:
        case FRUSTUM_DRAW_MODE_ALL: 
            _frustumRenderModeAction->setText("Render Mode - All");
            break;
        case FRUSTUM_DRAW_MODE_VECTORS: 
            _frustumRenderModeAction->setText("Render Mode - Vectors");
            break;
        case FRUSTUM_DRAW_MODE_PLANES:
            _frustumRenderModeAction->setText("Render Mode - Planes");
            break;
        case FRUSTUM_DRAW_MODE_NEAR_PLANE: 
            _frustumRenderModeAction->setText("Render Mode - Near");
            break;
        case FRUSTUM_DRAW_MODE_FAR_PLANE: 
            _frustumRenderModeAction->setText("Render Mode - Far");
            break; 
    }
}

void Application::initDisplay() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glShadeModel (GL_SMOOTH);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
}

void Application::init() {
    _voxels.init();
    _voxels.setViewerAvatar(&_myAvatar);
    _voxels.setCamera(&_myCamera);
    
    _environment.init();
    
    _handControl.setScreenDimensions(_glWidget->width(), _glWidget->height());

    _headMouseX = _glWidget->width()/2;
    _headMouseY = _glWidget->height()/2; 

    _stars.readInput(STAR_FILE, STAR_CACHE_FILE, 0);
  
    _myAvatar.setPosition(START_LOCATION);
    Camera::CameraFollowingAttributes a;            
    a.upShift   = -0.2f;
    a.distance  = 1.5f;
    a.tightness = 8.0f;
    _myCamera.setMode(CAMERA_MODE_THIRD_PERSON, a);
    _myAvatar.setDisplayingHead(true);   
    
    OculusManager::connect();
    
    gettimeofday(&_timerStart, NULL);
    gettimeofday(&_lastTimeIdle, NULL);
}

void Application::updateAvatar(float deltaTime) {
    // Update my avatar's head position from gyros
    _myAvatar.updateHeadFromGyros(deltaTime, &_serialPort, &_gravity);

    //  Grab latest readings from the gyros
    float measuredPitchRate = _serialPort.getLastPitchRate();
    float measuredYawRate = _serialPort.getLastYawRate();
    
    //  Update gyro-based mouse (X,Y on screen)
    const float MIN_MOUSE_RATE = 1.0;
    const float HORIZONTAL_PIXELS_PER_DEGREE = 2880.f / 45.f;
    const float VERTICAL_PIXELS_PER_DEGREE = 1800.f / 30.f;
    if (powf(measuredYawRate * measuredYawRate +
             measuredPitchRate * measuredPitchRate, 0.5) > MIN_MOUSE_RATE)
    {
        _headMouseX += measuredYawRate * HORIZONTAL_PIXELS_PER_DEGREE * deltaTime;
        _headMouseY -= measuredPitchRate * VERTICAL_PIXELS_PER_DEGREE * deltaTime;
    }
    _headMouseX = max(_headMouseX, 0);
    _headMouseX = min(_headMouseX, _glWidget->width());
    _headMouseY = max(_headMouseY, 0);
    _headMouseY = min(_headMouseY, _glWidget->height());
    
    //  Update head and body pitch and yaw based on measured gyro rates
    if (_gyroLook->isChecked()) {
        // Render Yaw
        float renderYawSpring = fabs(_headMouseX - _glWidget->width() / 2.f) / (_glWidget->width() / 2.f);
        const float RENDER_YAW_MULTIPLY = 4.f;
        _myAvatar.setRenderYaw((1.f - renderYawSpring * deltaTime) * _myAvatar.getRenderYaw() +
                              renderYawSpring * deltaTime * -_myAvatar.getHeadYaw() * RENDER_YAW_MULTIPLY);
        // Render Pitch
        float renderPitchSpring = fabs(_headMouseY - _glWidget->height() / 2.f) / (_glWidget->height() / 2.f);
        const float RENDER_PITCH_MULTIPLY = 4.f;
        _myAvatar.setRenderPitch((1.f - renderPitchSpring * deltaTime) * _myAvatar.getRenderPitch() +
                                renderPitchSpring * deltaTime * -_myAvatar.getHeadPitch() * RENDER_PITCH_MULTIPLY);
    }
    
    
    if (USING_MOUSE_VIEW_SHIFT)
    {
        //make it so that when your mouse hits the edge of the screen, the camera shifts
        float rightBoundary  = (float)_glWidget->width()  - MOUSE_VIEW_SHIFT_YAW_MARGIN;
        float bottomBoundary = (float)_glWidget->height() - MOUSE_VIEW_SHIFT_PITCH_MARGIN;
        
        if (_mouseX > rightBoundary) {
            float f = (_mouseX - rightBoundary) / ( (float)_glWidget->width() - rightBoundary);
            _mouseViewShiftYaw += MOUSE_VIEW_SHIFT_RATE * f * deltaTime;
            if (_mouseViewShiftYaw > MOUSE_VIEW_SHIFT_YAW_LIMIT) { _mouseViewShiftYaw = MOUSE_VIEW_SHIFT_YAW_LIMIT; }
        } else if (_mouseX < MOUSE_VIEW_SHIFT_YAW_MARGIN) {
            float f = 1.0 - (_mouseX / MOUSE_VIEW_SHIFT_YAW_MARGIN);
            _mouseViewShiftYaw -= MOUSE_VIEW_SHIFT_RATE * f * deltaTime;
            if (_mouseViewShiftYaw < -MOUSE_VIEW_SHIFT_YAW_LIMIT) { _mouseViewShiftYaw = -MOUSE_VIEW_SHIFT_YAW_LIMIT; }
        }
        if (_mouseY < MOUSE_VIEW_SHIFT_PITCH_MARGIN) {
            float f = 1.0 - (_mouseY / MOUSE_VIEW_SHIFT_PITCH_MARGIN);
            _mouseViewShiftPitch += MOUSE_VIEW_SHIFT_RATE * f * deltaTime;
            if (_mouseViewShiftPitch > MOUSE_VIEW_SHIFT_PITCH_LIMIT ) { _mouseViewShiftPitch = MOUSE_VIEW_SHIFT_PITCH_LIMIT; }
        }
        else if (_mouseY > bottomBoundary) {
            float f = (_mouseY - bottomBoundary) / ((float)_glWidget->height() - bottomBoundary);
            _mouseViewShiftPitch -= MOUSE_VIEW_SHIFT_RATE * f * deltaTime;
            if (_mouseViewShiftPitch < -MOUSE_VIEW_SHIFT_PITCH_LIMIT) { _mouseViewShiftPitch = -MOUSE_VIEW_SHIFT_PITCH_LIMIT; }
        }
    }
    
    if (OculusManager::isConnected()) {
        float yaw, pitch, roll;
        OculusManager::getEulerAngles(yaw, pitch, roll);
        
        _myAvatar.setHeadYaw(-yaw);
        _myAvatar.setHeadPitch(pitch);
        _myAvatar.setHeadRoll(roll);
    }
    
    //  Get audio loudness data from audio input device
    #ifndef _WIN32
        _myAvatar.setLoudness(_audio.getLastInputLoudness());
    #endif

    // Update Avatar with latest camera and view frustum data...
    // NOTE: we get this from the view frustum, to make it simpler, since the
    // loadViewFrumstum() method will get the correct details from the camera
    // We could optimize this to not actually load the viewFrustum, since we don't
    // actually need to calculate the view frustum planes to send these details 
    // to the server.
    loadViewFrustum(_viewFrustum);
    _myAvatar.setCameraPosition(_viewFrustum.getPosition());
    _myAvatar.setCameraDirection(_viewFrustum.getDirection());
    _myAvatar.setCameraUp(_viewFrustum.getUp());
    _myAvatar.setCameraRight(_viewFrustum.getRight());
    _myAvatar.setCameraFov(_viewFrustum.getFieldOfView());
    _myAvatar.setCameraAspectRatio(_viewFrustum.getAspectRatio());
    _myAvatar.setCameraNearClip(_viewFrustum.getNearClip());
    _myAvatar.setCameraFarClip(_viewFrustum.getFarClip());
    
    AgentList* agentList = AgentList::getInstance();
    if (agentList->getOwnerID() != UNKNOWN_AGENT_ID) {
        // if I know my ID, send head/hand data to the avatar mixer and voxel server
        unsigned char broadcastString[200];
        unsigned char* endOfBroadcastStringWrite = broadcastString;
        
        *(endOfBroadcastStringWrite++) = PACKET_HEADER_HEAD_DATA;
        endOfBroadcastStringWrite += packAgentId(endOfBroadcastStringWrite, agentList->getOwnerID());
        
        endOfBroadcastStringWrite += _myAvatar.getBroadcastData(endOfBroadcastStringWrite);
        
        const char broadcastReceivers[2] = {AGENT_TYPE_VOXEL, AGENT_TYPE_AVATAR_MIXER};
        AgentList::getInstance()->broadcastToAgents(broadcastString, endOfBroadcastStringWrite - broadcastString, broadcastReceivers, sizeof(broadcastReceivers));
    }

    // If I'm in paint mode, send a voxel out to VOXEL server agents.
    if (_paintOn) {
    
        glm::vec3 avatarPos = _myAvatar.getPosition();

        // For some reason, we don't want to flip X and Z here.
        _paintingVoxel.x = avatarPos.x / 10.0;
        _paintingVoxel.y = avatarPos.y / 10.0;
        _paintingVoxel.z = avatarPos.z / 10.0;
        
        if (_paintingVoxel.x >= 0.0 && _paintingVoxel.x <= 1.0 &&
            _paintingVoxel.y >= 0.0 && _paintingVoxel.y <= 1.0 &&
            _paintingVoxel.z >= 0.0 && _paintingVoxel.z <= 1.0) {

            PACKET_HEADER message = (_destructiveAddVoxel->isChecked() ?
                PACKET_HEADER_SET_VOXEL_DESTRUCTIVE : PACKET_HEADER_SET_VOXEL);
            sendVoxelEditMessage(message, _paintingVoxel);
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////
// loadViewFrustum()
//
// Description: this will load the view frustum bounds for EITHER the head
//                 or the "myCamera". 
//
void Application::loadViewFrustum(ViewFrustum& viewFrustum) {
    // We will use these below, from either the camera or head vectors calculated above    
    glm::vec3 position;
    glm::vec3 direction;
    glm::vec3 up;
    glm::vec3 right;
    float fov, nearClip, farClip;
    
    // Camera or Head?
    if (_cameraFrustum->isChecked()) {
        position = _myCamera.getPosition();
    } else {
        position = _myAvatar.getHeadPosition();
    }
    
    fov         = _myCamera.getFieldOfView();
    nearClip    = _myCamera.getNearClip();
    farClip     = _myCamera.getFarClip();

    Orientation o = _myCamera.getOrientation();

    direction   = o.getFront();
    up          = o.getUp();
    right       = o.getRight();

    /*
    printf("position.x=%f, position.y=%f, position.z=%f\n", position.x, position.y, position.z);
    printf("yaw=%f, pitch=%f, roll=%f\n", yaw,pitch,roll);
    printf("direction.x=%f, direction.y=%f, direction.z=%f\n", direction.x, direction.y, direction.z);
    printf("up.x=%f, up.y=%f, up.z=%f\n", up.x, up.y, up.z);
    printf("right.x=%f, right.y=%f, right.z=%f\n", right.x, right.y, right.z);
    printf("fov=%f\n", fov);
    printf("nearClip=%f\n", nearClip);
    printf("farClip=%f\n", farClip);
    */
    
    // Set the viewFrustum up with the correct position and orientation of the camera    
    viewFrustum.setPosition(position);
    viewFrustum.setOrientation(direction,up,right);
    
    // Also make sure it's got the correct lens details from the camera
    viewFrustum.setFieldOfView(fov);
    viewFrustum.setNearClip(nearClip);
    viewFrustum.setFarClip(farClip);

    // Ask the ViewFrustum class to calculate our corners
    viewFrustum.calculate();
}

// this shader is an adaptation (HLSL -> GLSL, removed conditional) of the one in the Oculus sample
// code (Samples/OculusRoomTiny/RenderTiny_D3D1X_Device.cpp), which is under the Apache license
// (http://www.apache.org/licenses/LICENSE-2.0)
static const char* DISTORTION_FRAGMENT_SHADER =
    "#version 120\n"
    "uniform sampler2D texture;"
    "uniform vec2 lensCenter;"
    "uniform vec2 screenCenter;"
    "uniform vec2 scale;"
    "uniform vec2 scaleIn;"
    "uniform vec4 hmdWarpParam;"
    "vec2 hmdWarp(vec2 in01) {"
    "   vec2 theta = (in01 - lensCenter) * scaleIn;"
    "   float rSq = theta.x * theta.x + theta.y * theta.y;"
    "   vec2 theta1 = theta * (hmdWarpParam.x + hmdWarpParam.y * rSq + "
    "                 hmdWarpParam.z * rSq * rSq + hmdWarpParam.w * rSq * rSq * rSq);"
    "   return lensCenter + scale * theta1;"
    "}"
    "void main(void) {"
    "   vec2 tc = hmdWarp(gl_TexCoord[0].st);"
    "   vec2 below = step(screenCenter.st + vec2(-0.25, -0.5), tc.st);"
    "   vec2 above = vec2(1.0, 1.0) - step(screenCenter.st + vec2(0.25, 0.5), tc.st);"
    "   gl_FragColor = mix(vec4(0.0, 0.0, 0.0, 1.0), texture2D(texture, tc), "
    "       above.s * above.t * below.s * below.t);"
    "}";
    
void Application::displayOculus(Camera& whichCamera) {
    // magic numbers ahoy! in order to avoid pulling in the Oculus utility library that calculates
    // the rendering parameters from the hardware stats, i just folded their calculations into
    // constants using the stats for the current-model hardware as contained in the SDK file
    // LibOVR/Src/Util/Util_Render_Stereo.cpp

    // eye 

    // render the left eye view to the left side of the screen
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glTranslatef(0.151976, 0, 0); // +h, see Oculus SDK docs p. 26
    gluPerspective(whichCamera.getFieldOfView(), whichCamera.getAspectRatio(),
        whichCamera.getNearClip(), whichCamera.getFarClip());
    glTranslatef(0.032, 0, 0); // dip/2, see p. 27
    
    glMatrixMode(GL_MODELVIEW);
    glViewport(0, 0, _glWidget->width() / 2, _glWidget->height());
    displaySide(whichCamera);

    // and the right eye to the right side
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glTranslatef(-0.151976, 0, 0); // -h
    gluPerspective(whichCamera.getFieldOfView(), whichCamera.getAspectRatio(),
        whichCamera.getNearClip(), whichCamera.getFarClip());
    glTranslatef(-0.032, 0, 0);
    
    glMatrixMode(GL_MODELVIEW);
    glViewport(_glWidget->width() / 2, 0, _glWidget->width() / 2, _glWidget->height());
    displaySide(whichCamera);

    glPopMatrix();
    
    // restore our normal viewport
    glViewport(0, 0, _glWidget->width(), _glWidget->height());

    if (_oculusTextureID == 0) {
        glGenTextures(1, &_oculusTextureID);
        glBindTexture(GL_TEXTURE_2D, _oculusTextureID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _glWidget->width(), _glWidget->height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   
        
        _oculusProgram = new ProgramObject();
        _oculusProgram->attachFromSourceCode(GL_FRAGMENT_SHADER_ARB, DISTORTION_FRAGMENT_SHADER);
        _oculusProgram->link();
        
        _textureLocation = _oculusProgram->getUniformLocation("texture");
        _lensCenterLocation = _oculusProgram->getUniformLocation("lensCenter");
        _screenCenterLocation = _oculusProgram->getUniformLocation("screenCenter");
        _scaleLocation = _oculusProgram->getUniformLocation("scale");
        _scaleInLocation = _oculusProgram->getUniformLocation("scaleIn");
        _hmdWarpParamLocation = _oculusProgram->getUniformLocation("hmdWarpParam");
        
    } else {
        glBindTexture(GL_TEXTURE_2D, _oculusTextureID);
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, _glWidget->width(), _glWidget->height());

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, _glWidget->width(), 0, _glWidget->height());           
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    
    // for reference on setting these values, see SDK file Samples/OculusRoomTiny/RenderTiny_Device.cpp
    
    float scaleFactor = 1.0 / _oculusDistortionScale;
    float aspectRatio = (_glWidget->width() * 0.5) / _glWidget->height();
    
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    _oculusProgram->bind();
    _oculusProgram->setUniform(_textureLocation, 0);
    _oculusProgram->setUniform(_lensCenterLocation, 0.287994, 0.5); // see SDK docs, p. 29
    _oculusProgram->setUniform(_screenCenterLocation, 0.25, 0.5);
    _oculusProgram->setUniform(_scaleLocation, 0.25 * scaleFactor, 0.5 * scaleFactor * aspectRatio);
    _oculusProgram->setUniform(_scaleInLocation, 4, 2 / aspectRatio);
    _oculusProgram->setUniform(_hmdWarpParamLocation, 1.0, 0.22, 0.24, 0);

    glColor3f(1, 0, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2f(0, 0);
    glTexCoord2f(0.5, 0);
    glVertex2f(_glWidget->width()/2, 0);
    glTexCoord2f(0.5, 1);
    glVertex2f(_glWidget->width() / 2, _glWidget->height());
    glTexCoord2f(0, 1);
    glVertex2f(0, _glWidget->height());
    glEnd();
    
    _oculusProgram->setUniform(_lensCenterLocation, 0.787994, 0.5);
    _oculusProgram->setUniform(_screenCenterLocation, 0.75, 0.5);
    
    glBegin(GL_QUADS);
    glTexCoord2f(0.5, 0);
    glVertex2f(_glWidget->width() / 2, 0);
    glTexCoord2f(1, 0);
    glVertex2f(_glWidget->width(), 0);
    glTexCoord2f(1, 1);
    glVertex2f(_glWidget->width(), _glWidget->height());
    glTexCoord2f(0.5, 1);
    glVertex2f(_glWidget->width() / 2, _glWidget->height());
    glEnd();
    
    glEnable(GL_BLEND);           
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    _oculusProgram->release();
    
    glPopMatrix();
}

void Application::displaySide(Camera& whichCamera) {
    glPushMatrix();
    
    if (_renderStarsOn->isChecked()) {
        // should be the first rendering pass - w/o depth buffer / lighting

        // compute starfield alpha based on distance from atmosphere
        float alpha = 1.0f;
        if (_renderAtmosphereOn->isChecked()) {
            float height = glm::distance(whichCamera.getPosition(), _environment.getAtmosphereCenter());
            if (height < _environment.getAtmosphereInnerRadius()) {
                alpha = 0.0f;
                
            } else if (height < _environment.getAtmosphereOuterRadius()) {
                alpha = (height - _environment.getAtmosphereInnerRadius()) /
                    (_environment.getAtmosphereOuterRadius() - _environment.getAtmosphereInnerRadius());
            }
        }

        // finally render the starfield
        _stars.render(whichCamera.getFieldOfView(), whichCamera.getAspectRatio(), whichCamera.getNearClip(), alpha);
    }

    // draw the sky dome
    if (_renderAtmosphereOn->isChecked()) {
        _environment.renderAtmosphere(whichCamera);
    }
    
    glEnable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    
    // draw a red sphere  
    float sphereRadius = 0.25f;
    glColor3f(1,0,0);
    glPushMatrix();
        glutSolidSphere(sphereRadius, 15, 15);
    glPopMatrix();

    //draw a grid ground plane....
    drawGroundPlaneGrid(10.f);
    
    //  Draw voxels
    if (_renderVoxels->isChecked()) {
        _voxels.render();
    }
    
    // indicate what we'll be adding/removing in mouse mode, if anything
    if (_mouseVoxel.s != 0) {
        glDisable(GL_LIGHTING);
        glPushMatrix();
        if (_addVoxelMode->isChecked()) {
            // use a contrasting color so that we can see what we're doing
            glColor3ub(_mouseVoxel.red + 128, _mouseVoxel.green + 128, _mouseVoxel.blue + 128);
        } else {
            glColor3ub(_mouseVoxel.red, _mouseVoxel.green, _mouseVoxel.blue);
        }
        glScalef(TREE_SCALE, TREE_SCALE, TREE_SCALE);
        glTranslatef(_mouseVoxel.x + _mouseVoxel.s*0.5f,
                     _mouseVoxel.y + _mouseVoxel.s*0.5f,
                     _mouseVoxel.z + _mouseVoxel.s*0.5f);
        glLineWidth(4.0f);
        glutWireCube(_mouseVoxel.s);
        glLineWidth(1.0f);
        glPopMatrix();
        glEnable(GL_LIGHTING);
    }
    
    if (_renderAvatarsOn->isChecked()) {
        //  Render avatars of other agents
        AgentList* agentList = AgentList::getInstance();
        agentList->lock();
        for (AgentList::iterator agent = agentList->begin(); agent != agentList->end(); agent++) {
            if (agent->getLinkedData() != NULL && agent->getType() == AGENT_TYPE_AVATAR) {
                Avatar *avatar = (Avatar *)agent->getLinkedData();
                avatar->render(0, _myCamera.getPosition());
            }
        }
        agentList->unlock();
        
        // Render my own Avatar 
        _myAvatar.render(_lookingInMirror, _myCamera.getPosition());
    }
    
    //  Render the world box
    if (!_lookingInMirror->isChecked() && _renderStatsOn->isChecked()) { render_world_box(); }
    
    // brad's frustum for debugging
    if (_frustumOn->isChecked()) renderViewFrustum(_viewFrustum);
    
    glPopMatrix();
}

void Application::displayOverlay() {
    //  Render 2D overlay:  I/O level bar graphs and text  
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
        glLoadIdentity(); 
        gluOrtho2D(0, _glWidget->width(), _glWidget->height(), 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
    
        #ifndef _WIN32
        _audio.render(_glWidget->width(), _glWidget->height());
        _audioScope.render(20, _glWidget->height() - 200);
        #endif

       //noiseTest(_glWidget->width(), _glWidget->height());
    
    if (DISPLAY_HEAD_MOUSE && !_lookingInMirror->isChecked() && USING_INVENSENSE_MPU9150) {
            //  Display small target box at center or head mouse target that can also be used to measure LOD
            glColor3f(1.0, 1.0, 1.0);
            glDisable(GL_LINE_SMOOTH);
            const int PIXEL_BOX = 20;
            glBegin(GL_LINE_STRIP);
            glVertex2f(_headMouseX - PIXEL_BOX/2, _headMouseY - PIXEL_BOX/2);
            glVertex2f(_headMouseX + PIXEL_BOX/2, _headMouseY - PIXEL_BOX/2);
            glVertex2f(_headMouseX + PIXEL_BOX/2, _headMouseY + PIXEL_BOX/2);
            glVertex2f(_headMouseX - PIXEL_BOX/2, _headMouseY + PIXEL_BOX/2);
            glVertex2f(_headMouseX - PIXEL_BOX/2, _headMouseY - PIXEL_BOX/2);
            glEnd();            
            glEnable(GL_LINE_SMOOTH);
        }
        
    //  Show detected levels from the serial I/O ADC channel sensors
    if (_displayLevels) _serialPort.renderLevels(_glWidget->width(), _glWidget->height());
    
    //  Display stats and log text onscreen
    glLineWidth(1.0f);
    glPointSize(1.0f);
    
    if (_renderStatsOn->isChecked()) { displayStats(); }
    if (_logOn->isChecked()) { logger.render(_glWidget->width(), _glWidget->height()); }

    //  Show chat entry field
    if (_chatEntryOn) {
        _chatEntry.render(_glWidget->width(), _glWidget->height());
    }

    //  Stats at upper right of screen about who domain server is telling us about
    glPointSize(1.0f);
    char agents[100];
    
    AgentList* agentList = AgentList::getInstance();
    int totalAvatars = 0, totalServers = 0;
    
    for (AgentList::iterator agent = agentList->begin(); agent != agentList->end(); agent++) {
        agent->getType() == AGENT_TYPE_AVATAR ? totalAvatars++ : totalServers++;
    }
    
    sprintf(agents, "Servers: %d, Avatars: %d\n", totalServers, totalAvatars);
    drawtext(_glWidget->width() - 150, 20, 0.10, 0, 1.0, 0, agents, 1, 0, 0);
    
    if (_paintOn) {
    
        char paintMessage[100];
        sprintf(paintMessage,"Painting (%.3f,%.3f,%.3f/%.3f/%d,%d,%d)",
            _paintingVoxel.x, _paintingVoxel.y, _paintingVoxel.z, _paintingVoxel.s,
            (unsigned int)_paintingVoxel.red, (unsigned int)_paintingVoxel.green, (unsigned int)_paintingVoxel.blue);
        drawtext(_glWidget->width() - 350, 50, 0.10, 0, 1.0, 0, paintMessage, 1, 1, 0);
    }
    
    glPopMatrix();
}

void Application::displayStats() {
    int statsVerticalOffset = 8;

    char stats[200];
    sprintf(stats, "%3.0f FPS, %d Pkts/sec, %3.2f Mbps", 
            _fps, _packetsPerSecond,  (float)_bytesPerSecond * 8.f / 1000000.f);
    drawtext(10, statsVerticalOffset + 15, 0.10f, 0, 1.0, 0, stats);
    
    std::stringstream voxelStats;
    voxelStats.precision(4);
    voxelStats << "Voxels Rendered: " << _voxels.getVoxelsRendered() / 1000.f << "K Updated: " << _voxels.getVoxelsUpdated()/1000.f << "K";
    drawtext(10, statsVerticalOffset + 230, 0.10f, 0, 1.0, 0, (char *)voxelStats.str().c_str());
    
    voxelStats.str("");
    voxelStats << "Voxels Created: " << _voxels.getVoxelsCreated() / 1000.f << "K (" << _voxels.getVoxelsCreatedPerSecondAverage() / 1000.f
    << "Kps) ";
    drawtext(10, statsVerticalOffset + 250, 0.10f, 0, 1.0, 0, (char *)voxelStats.str().c_str());
    
    voxelStats.str("");
    voxelStats << "Voxels Colored: " << _voxels.getVoxelsColored() / 1000.f << "K (" << _voxels.getVoxelsColoredPerSecondAverage() / 1000.f
    << "Kps) ";
    drawtext(10, statsVerticalOffset + 270, 0.10f, 0, 1.0, 0, (char *)voxelStats.str().c_str());
    
    voxelStats.str("");
    voxelStats << "Voxel Bits Read: " << _voxels.getVoxelsBytesRead() * 8.f / 1000000.f 
    << "M (" << _voxels.getVoxelsBytesReadPerSecondAverage() * 8.f / 1000000.f << " Mbps)";
    drawtext(10, statsVerticalOffset + 290,0.10f, 0, 1.0, 0, (char *)voxelStats.str().c_str());

    voxelStats.str("");
    float voxelsBytesPerColored = _voxels.getVoxelsColored()
        ? ((float) _voxels.getVoxelsBytesRead() / _voxels.getVoxelsColored())
        : 0;
    
    voxelStats << "Voxels Bits per Colored: " << voxelsBytesPerColored * 8;
    drawtext(10, statsVerticalOffset + 310, 0.10f, 0, 1.0, 0, (char *)voxelStats.str().c_str());
    
    Agent *avatarMixer = AgentList::getInstance()->soloAgentOfType(AGENT_TYPE_AVATAR_MIXER);
    char avatarMixerStats[200];
    
    if (avatarMixer) {
        sprintf(avatarMixerStats, "Avatar Mixer: %.f kbps, %.f pps",
                roundf(avatarMixer->getAverageKilobitsPerSecond()),
                roundf(avatarMixer->getAveragePacketsPerSecond()));
    } else {
        sprintf(avatarMixerStats, "No Avatar Mixer");
    }
    
    drawtext(10, statsVerticalOffset + 330, 0.10f, 0, 1.0, 0, avatarMixerStats);
    
    if (_perfStatsOn) {
        // Get the PerfStats group details. We need to allocate and array of char* long enough to hold 1+groups
        char** perfStatLinesArray = new char*[PerfStat::getGroupCount()+1];
        int lines = PerfStat::DumpStats(perfStatLinesArray);
        int atZ = 150; // arbitrary place on screen that looks good
        for (int line=0; line < lines; line++) {
            drawtext(10, statsVerticalOffset + atZ, 0.10f, 0, 1.0, 0, perfStatLinesArray[line]);
            delete perfStatLinesArray[line]; // we're responsible for cleanup
            perfStatLinesArray[line]=NULL;
            atZ+=20; // height of a line
        }
        delete []perfStatLinesArray; // we're responsible for cleanup
    }
}

/////////////////////////////////////////////////////////////////////////////////////
// renderViewFrustum()
//
// Description: this will render the view frustum bounds for EITHER the head
//                 or the "myCamera". 
//
// Frustum rendering mode. For debug purposes, we allow drawing the frustum in a couple of different ways.
// We can draw it with each of these parts:
//    * Origin Direction/Up/Right vectors - these will be drawn at the point of the camera
//    * Near plane - this plane is drawn very close to the origin point.
//    * Right/Left planes - these two planes are drawn between the near and far planes.
//    * Far plane - the plane is drawn in the distance.
// Modes - the following modes, will draw the following parts.
//    * All - draws all the parts listed above
//    * Planes - draws the planes but not the origin vectors
//    * Origin Vectors - draws the origin vectors ONLY
//    * Near Plane - draws only the near plane
//    * Far Plane - draws only the far plane
void Application::renderViewFrustum(ViewFrustum& viewFrustum) {
    // Load it with the latest details!
    loadViewFrustum(viewFrustum);
    
    glm::vec3 position  = viewFrustum.getPosition();
    glm::vec3 direction = viewFrustum.getDirection();
    glm::vec3 up        = viewFrustum.getUp();
    glm::vec3 right     = viewFrustum.getRight();
    
    //  Get ready to draw some lines
    glDisable(GL_LIGHTING);
    glColor4f(1.0, 1.0, 1.0, 1.0);
    glLineWidth(1.0);
    glBegin(GL_LINES);

    if (_frustumDrawingMode == FRUSTUM_DRAW_MODE_ALL || _frustumDrawingMode == FRUSTUM_DRAW_MODE_VECTORS) {
        // Calculate the origin direction vectors
        glm::vec3 lookingAt      = position + (direction * 0.2f);
        glm::vec3 lookingAtUp    = position + (up * 0.2f);
        glm::vec3 lookingAtRight = position + (right * 0.2f);

        // Looking At = white
        glColor3f(1,1,1);
        glVertex3f(position.x, position.y, position.z);
        glVertex3f(lookingAt.x, lookingAt.y, lookingAt.z);

        // Looking At Up = purple
        glColor3f(1,0,1);
        glVertex3f(position.x, position.y, position.z);
        glVertex3f(lookingAtUp.x, lookingAtUp.y, lookingAtUp.z);

        // Looking At Right = cyan
        glColor3f(0,1,1);
        glVertex3f(position.x, position.y, position.z);
        glVertex3f(lookingAtRight.x, lookingAtRight.y, lookingAtRight.z);
    }

    if (_frustumDrawingMode == FRUSTUM_DRAW_MODE_ALL || _frustumDrawingMode == FRUSTUM_DRAW_MODE_PLANES
            || _frustumDrawingMode == FRUSTUM_DRAW_MODE_NEAR_PLANE) {
        // Drawing the bounds of the frustum
        // viewFrustum.getNear plane - bottom edge 
        glColor3f(1,0,0);
        glVertex3f(viewFrustum.getNearBottomLeft().x, viewFrustum.getNearBottomLeft().y, viewFrustum.getNearBottomLeft().z);
        glVertex3f(viewFrustum.getNearBottomRight().x, viewFrustum.getNearBottomRight().y, viewFrustum.getNearBottomRight().z);

        // viewFrustum.getNear plane - top edge
        glVertex3f(viewFrustum.getNearTopLeft().x, viewFrustum.getNearTopLeft().y, viewFrustum.getNearTopLeft().z);
        glVertex3f(viewFrustum.getNearTopRight().x, viewFrustum.getNearTopRight().y, viewFrustum.getNearTopRight().z);

        // viewFrustum.getNear plane - right edge
        glVertex3f(viewFrustum.getNearBottomRight().x, viewFrustum.getNearBottomRight().y, viewFrustum.getNearBottomRight().z);
        glVertex3f(viewFrustum.getNearTopRight().x, viewFrustum.getNearTopRight().y, viewFrustum.getNearTopRight().z);

        // viewFrustum.getNear plane - left edge
        glVertex3f(viewFrustum.getNearBottomLeft().x, viewFrustum.getNearBottomLeft().y, viewFrustum.getNearBottomLeft().z);
        glVertex3f(viewFrustum.getNearTopLeft().x, viewFrustum.getNearTopLeft().y, viewFrustum.getNearTopLeft().z);
    }

    if (_frustumDrawingMode == FRUSTUM_DRAW_MODE_ALL || _frustumDrawingMode == FRUSTUM_DRAW_MODE_PLANES
            || _frustumDrawingMode == FRUSTUM_DRAW_MODE_FAR_PLANE) {
        // viewFrustum.getFar plane - bottom edge 
        glColor3f(0,1,0); // GREEN!!!
        glVertex3f(viewFrustum.getFarBottomLeft().x, viewFrustum.getFarBottomLeft().y, viewFrustum.getFarBottomLeft().z);
        glVertex3f(viewFrustum.getFarBottomRight().x, viewFrustum.getFarBottomRight().y, viewFrustum.getFarBottomRight().z);

        // viewFrustum.getFar plane - top edge
        glVertex3f(viewFrustum.getFarTopLeft().x, viewFrustum.getFarTopLeft().y, viewFrustum.getFarTopLeft().z);
        glVertex3f(viewFrustum.getFarTopRight().x, viewFrustum.getFarTopRight().y, viewFrustum.getFarTopRight().z);

        // viewFrustum.getFar plane - right edge
        glVertex3f(viewFrustum.getFarBottomRight().x, viewFrustum.getFarBottomRight().y, viewFrustum.getFarBottomRight().z);
        glVertex3f(viewFrustum.getFarTopRight().x, viewFrustum.getFarTopRight().y, viewFrustum.getFarTopRight().z);

        // viewFrustum.getFar plane - left edge
        glVertex3f(viewFrustum.getFarBottomLeft().x, viewFrustum.getFarBottomLeft().y, viewFrustum.getFarBottomLeft().z);
        glVertex3f(viewFrustum.getFarTopLeft().x, viewFrustum.getFarTopLeft().y, viewFrustum.getFarTopLeft().z);
    }

    if (_frustumDrawingMode == FRUSTUM_DRAW_MODE_ALL || _frustumDrawingMode == FRUSTUM_DRAW_MODE_PLANES) {
        // RIGHT PLANE IS CYAN
        // right plane - bottom edge - viewFrustum.getNear to distant 
        glColor3f(0,1,1);
        glVertex3f(viewFrustum.getNearBottomRight().x, viewFrustum.getNearBottomRight().y, viewFrustum.getNearBottomRight().z);
        glVertex3f(viewFrustum.getFarBottomRight().x, viewFrustum.getFarBottomRight().y, viewFrustum.getFarBottomRight().z);

        // right plane - top edge - viewFrustum.getNear to distant
        glVertex3f(viewFrustum.getNearTopRight().x, viewFrustum.getNearTopRight().y, viewFrustum.getNearTopRight().z);
        glVertex3f(viewFrustum.getFarTopRight().x, viewFrustum.getFarTopRight().y, viewFrustum.getFarTopRight().z);

        // LEFT PLANE IS BLUE
        // left plane - bottom edge - viewFrustum.getNear to distant
        glColor3f(0,0,1);
        glVertex3f(viewFrustum.getNearBottomLeft().x, viewFrustum.getNearBottomLeft().y, viewFrustum.getNearBottomLeft().z);
        glVertex3f(viewFrustum.getFarBottomLeft().x, viewFrustum.getFarBottomLeft().y, viewFrustum.getFarBottomLeft().z);

        // left plane - top edge - viewFrustum.getNear to distant
        glVertex3f(viewFrustum.getNearTopLeft().x, viewFrustum.getNearTopLeft().y, viewFrustum.getNearTopLeft().z);
        glVertex3f(viewFrustum.getFarTopLeft().x, viewFrustum.getFarTopLeft().y, viewFrustum.getFarTopLeft().z);
    }

    glEnd();
    glEnable(GL_LIGHTING);
}

void Application::setupPaintingVoxel() {
    glm::vec3 avatarPos = _myAvatar.getPosition();

    _paintingVoxel.x = avatarPos.z/-10.0;    // voxel space x is negative z head space
    _paintingVoxel.y = avatarPos.y/-10.0;  // voxel space y is negative y head space
    _paintingVoxel.z = avatarPos.x/-10.0;  // voxel space z is negative x head space
    _paintingVoxel.s = 1.0/256;
    
    shiftPaintingColor();
}

void Application::shiftPaintingColor() {
    // About the color of the paintbrush... first determine the dominant color
    _dominantColor = (_dominantColor + 1) % 3; // 0=red,1=green,2=blue
    _paintingVoxel.red   = (_dominantColor == 0) ? randIntInRange(200, 255) : randIntInRange(40, 100);
    _paintingVoxel.green = (_dominantColor == 1) ? randIntInRange(200, 255) : randIntInRange(40, 100);
    _paintingVoxel.blue  = (_dominantColor == 2) ? randIntInRange(200, 255) : randIntInRange(40, 100);
}

void Application::addVoxelUnderCursor() {
    if (_mouseVoxel.s != 0) {    
        PACKET_HEADER message = (_destructiveAddVoxel->isChecked() ?
            PACKET_HEADER_SET_VOXEL_DESTRUCTIVE : PACKET_HEADER_SET_VOXEL);
        sendVoxelEditMessage(message, _mouseVoxel);
        
        // create the voxel locally so it appears immediately            
        _voxels.createVoxel(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s,
                           _mouseVoxel.red, _mouseVoxel.green, _mouseVoxel.blue, _destructiveAddVoxel->isChecked());
    
        // remember the position for drag detection
        _lastMouseVoxelPos = glm::vec3(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z);
    }
}

void Application::deleteVoxelUnderCursor() {
    if (_mouseVoxel.s != 0) {
        sendVoxelEditMessage(PACKET_HEADER_ERASE_VOXEL, _mouseVoxel);
        
        // delete the voxel locally so it disappears immediately            
        _voxels.deleteVoxelAt(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z, _mouseVoxel.s);
        
        // remember the position for drag detection
        _lastMouseVoxelPos = glm::vec3(_mouseVoxel.x, _mouseVoxel.y, _mouseVoxel.z);
    }
}

void Application::resetSensors() {
    _myAvatar.setPosition(START_LOCATION);
    _headMouseX = _glWidget->width() / 2;
    _headMouseY = _glWidget->height() / 2;
    
    if (_serialPort.active) {
        _serialPort.resetAverages();
    } 
    _myAvatar.reset();
}

static void setShortcutsEnabled(QWidget* widget, bool enabled) {
    foreach (QAction* action, widget->actions()) {
        QKeySequence shortcut = action->shortcut();
        if (!shortcut.isEmpty() && (shortcut[0] & (Qt::CTRL | Qt::ALT | Qt::META)) == 0) {
            // it's a shortcut that may coincide with a "regular" key, so switch its context
            action->setShortcutContext(enabled ? Qt::WindowShortcut : Qt::WidgetShortcut);
        }
    }
    foreach (QObject* child, widget->children()) {
        if (child->isWidgetType()) {
            setShortcutsEnabled(static_cast<QWidget*>(child), enabled);
        }
    }
}

void Application::setMenuShortcutsEnabled(bool enabled) {
    setShortcutsEnabled(_window->menuBar(), enabled);
}

// when QActionGroup is set to non-exclusive, it doesn't return anything as checked;
// hence, we must check ourselves
QAction* Application::checkedVoxelModeAction() const {
    foreach (QAction* action, _voxelModeActions->actions()) {
        if (action->isChecked()) {
            return action;
        }
    }
    return 0;
}

void Application::attachNewHeadToAgent(Agent *newAgent) {
    if (newAgent->getLinkedData() == NULL) {
        newAgent->setLinkedData(new Avatar(false));
    }
}

//  Receive packets from other agents/servers and decide what to do with them!
void* Application::networkReceive(void* args) {
    sockaddr senderAddress;
    ssize_t bytesReceived;
    
    Application* app = static_cast<Application*>(QCoreApplication::instance());
    while (!app->_stopNetworkReceiveThread) {
        // check to see if the UI thread asked us to kill the voxel tree. since we're the only thread allowed to do that
        if (app->_wantToKillLocalVoxels) {
            app->_voxels.killLocalVoxels();
            app->_wantToKillLocalVoxels = false;
        }
    
        if (AgentList::getInstance()->getAgentSocket()->receive(&senderAddress, app->_incomingPacket, &bytesReceived)) {
            app->_packetCount++;
            app->_bytesCount += bytesReceived;
            
            switch (app->_incomingPacket[0]) {
                case PACKET_HEADER_TRANSMITTER_DATA_V1:
                    //  Process UDP packets that are sent to the client from local sensor devices 
                    app->_myAvatar.processTransmitterData(app->_incomingPacket, bytesReceived);
                    break;
                case PACKET_HEADER_TRANSMITTER_DATA_V2:
                    float rotationRates[3];
                    float accelerations[3];
                    
                    memcpy(rotationRates, app->_incomingPacket + 2, sizeof(rotationRates));
                    memcpy(accelerations, app->_incomingPacket + 3 + sizeof(rotationRates), sizeof(accelerations));
                    
                    printf("The rotation: %f, %f, %f\n", rotationRates[0], rotationRates[1], rotationRates[2]);
                    break;
                case PACKET_HEADER_MIXED_AUDIO:
                    app->_audio.addReceivedAudioToBuffer(app->_incomingPacket, bytesReceived);
                    break;
                case PACKET_HEADER_VOXEL_DATA:
                case PACKET_HEADER_VOXEL_DATA_MONOCHROME:
                case PACKET_HEADER_Z_COMMAND:
                case PACKET_HEADER_ERASE_VOXEL:
                    app->_voxels.parseData(app->_incomingPacket, bytesReceived);
                    break;
                case PACKET_HEADER_ENVIRONMENT_DATA:
                    app->_environment.parseData(app->_incomingPacket, bytesReceived);
                    break;
                case PACKET_HEADER_BULK_AVATAR_DATA:
                    AgentList::getInstance()->processBulkAgentData(&senderAddress,
                                                                   app->_incomingPacket,
                                                                   bytesReceived);
                    break;
                default:
                    AgentList::getInstance()->processAgentData(&senderAddress, app->_incomingPacket, bytesReceived);
                    break;
            }
        } else if (!app->_enableNetworkThread) {
            break;
        }
    }
    
    if (app->_enableNetworkThread) {
        pthread_exit(0); 
    }
    return NULL; 
}
