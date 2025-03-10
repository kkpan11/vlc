/*****************************************************************************
 * Copyright (C) 2019 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifndef VIDEOSURFACE_HPP
#define VIDEOSURFACE_HPP

#include "widgets/native/viewblockingrectangle.hpp"
#include <QMutex>
#include <QRunnable>
#include <QPointer>
#include "qt.hpp"
#include "util/vlchotkeyconverter.hpp"

#include <vlc_threads.h>

extern "C" {
    typedef struct vlc_window vlc_window_t;
}

Q_MOC_INCLUDE( "maininterface/mainctx.hpp")

class MainCtx;

class WindowResizer :
    public QRunnable
{
public:
    WindowResizer(vlc_window_t* window);
    virtual ~WindowResizer();

    void run() override;
    void reportSize(float width, float height);
    void waitForCompletion();

private:
    vlc_mutex_t m_lock;
    vlc_cond_t m_cond;
    unsigned m_requestedWidth;
    unsigned m_requestedHeight;
    unsigned m_currentWidth;
    unsigned m_currentHeight;
    bool m_running;
    vlc_window_t* m_voutWindow;
};

class VideoSurfaceProvider : public QObject
{
    Q_OBJECT
public:
    VideoSurfaceProvider(QObject* parent = nullptr);
    virtual ~VideoSurfaceProvider() {}

    void enable(vlc_window_t* voutWindow);
    void disable();
    bool isEnabled();

    void setVideoEmbed(bool embed);
    bool hasVideoEmbed() const;

signals:
    void ctxChanged(MainCtx*);
    bool videoEnabledChanged(bool);
    bool hasVideoEmbedChanged(bool);
    void surfacePositionChanged(QPointF position);
    void surfaceSizeChanged(QSizeF size);
    void surfaceScaleChanged(qreal);

public slots:
    void onWindowClosed();
    void onMousePressed( int vlcButton );
    void onMouseReleased( int vlcButton );
    void onMouseDoubleClick( int vlcButton );
    void onMouseMoved( float x, float y );
    void onMouseWheeled(int vlcButton);
    void onKeyPressed(int key, Qt::KeyboardModifiers modifiers);
    void onSurfaceSizeChanged(QSizeF size);

protected:
    QMutex m_voutlock;
    vlc_window_t* m_voutWindow = nullptr;
    WindowResizer * m_resizer = nullptr;
    bool m_videoEmbed = false;
};


class VideoSurface : public ViewBlockingRectangle
{
    Q_OBJECT
    Q_PROPERTY(VideoSurfaceProvider* videoSurfaceProvider READ videoSurfaceProvider WRITE setVideoSurfaceProvider NOTIFY videoSurfaceProviderChanged FINAL)
    Q_PROPERTY(Qt::CursorShape cursorShape READ getCursorShape WRITE setCursorShape RESET unsetCursor FINAL)

public:
    VideoSurface( QQuickItem* parent = nullptr );

    VideoSurfaceProvider* videoSurfaceProvider() const { return m_provider; };
    void setVideoSurfaceProvider(VideoSurfaceProvider *newVideoSurfaceProvider);

protected:
    int qtMouseButton2VLC( Qt::MouseButton qtButton );

    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
#if QT_CONFIG(wheelevent)
    void wheelEvent(QWheelEvent *event) override;
#endif

    Qt::CursorShape getCursorShape() const;
    void setCursorShape(Qt::CursorShape);

    void updatePolish() override;

    void itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &value) override;

signals:
    void surfaceSizeChanged(QSizeF);
    void surfacePositionChanged(QPointF);
    void surfaceScaleChanged(qreal);

    void mousePressed( int vlcButton );
    void mouseReleased( int vlcButton );
    void mouseDblClicked( int vlcButton );
    void mouseMoved( float x, float y );
    void keyPressed(int key, Qt::KeyboardModifiers modifier);

    void videoSurfaceProviderChanged();

protected slots:
    void updateSurfacePosition();
    void updateSurfaceSize();
    void updateSurfaceScale();
    void updateSurface();

private:
    QPointF m_oldHoverPos;

    WheelToVLCConverter m_wheelEventConverter;

    QPointer<VideoSurfaceProvider> m_provider;

    bool m_sizeDirty = false;
    bool m_positionDirty = false;
    bool m_scaleDirty = false;
};

#endif // VIDEOSURFACE_HPP
