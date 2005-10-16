/***************************************************************************
                          clipmanager.h  -  Manages clips, makes sure that
			  		    they are
                             -------------------
    begin                : Wed Sep 17 08:36:16 GMT 2003
    copyright            : (C) 2003 by Jason Wood
    email                : jasonwood@blueyonder.co.uk
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef CLIPMANAGER_H
#define CLIPMANAGER_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif 

#include <qobject.h>
#include <qdom.h>

#include <kurl.h>

#include "docclipbaselist.h"

// forward declaration of the Kdenlive classes
class DocClipAVFile;
class KMacroCommand;
class KRender;
class KRenderManager;

/**	ClipManager manages a number of clips.
  *
  * The ClipManager class maintains a list of clips. This is the only place in the application where
  * DocClipAVFiles and other clip types should be created, and then only indirectly. The clip manager
  * makes sure that if a clip already exists, it is reused, and will "hide" whether a clip has just
  * been created or is new to the project away from the application code.
  *
  * The reason for maintaining AVFiles in this way is due to the non-blocking nature of Kdenlive. When
  * a clip is created, a request needs to be sent to the renderer to determine the properties of the
  * file. Until a reply is recieved, the clip remains in a non-determinate state. We use the clip manager
  * to prevent us sending unnecessary requests to the server - if we have already sent a request to find
  * a clips properties, we want to avoid sending it again.
  *  
  * @author Jason Wood <jasonwood@blueyonder.co.uk>
  */

class ClipManager : public QObject
{
Q_OBJECT
public:
	/** Constructor for the fileclass of the application */
	ClipManager(KRenderManager &renderManager, QWidget *parent=0, const char *name=0);
	/** Destructor for the fileclass of the application */
	~ClipManager();
	
	/** Find and return the AVFile with the url specified, or return null is no file matches. */
	DocClipBase *findClip(const KURL &file);
	
	/** find a specific clip, returns null if no clip matches */
	DocClipBase *findClip(const QDomElement &clip);

	/** Insert an AVFile with the given url. If the file is already in the file list, return 
	 * that instead. */
	DocClipBase *insertClip(const KURL &file);

	/** Insert a specific clip */
	DocClipBase *insertClip(const QDomElement &clip);

	/** Adds a temporary clip. This is a clip that does not "exist" in the project, but of which
	 * some stored information is required. */
	DocClipBase *addTemporaryClip(const QDomElement &clip);
	
	DocClipBase *addTemporaryClip(const KURL &file);

	/** Removes a clip from the clip manager. This method fails if the clip does not exist, or
	 * if it is referenced from anywhere, including the timeline or other clips.*/
	void removeClip(const KURL &file);
	void removeClip(const QDomElement &clip);

	/** Remove all clips from the clip manager. */
	void clear();
	
	void generateFromXML(KRender *render, const QDomElement &e);
	QDomDocument toXML(const QString &element);
signals:
 	/** This is signal is emitted whenever the clipList changes, either through the addition 
	 * or removal of a clip, or when an clip changes. */
  	void clipListUpdated();
	/** Emitted when a particular clip has changed in someway. E.g, it has recieved it's duration. */
	void clipChanged(DocClipBase *file);

public slots:
	/** This slot occurs when the File properties for an AV File have been returned by the renderer.
	The relevant AVFile can then be updated to the correct status. */
	void AVFilePropertiesArrived(const QMap<QString, QString> &properties);
	void AVImageArrived( const KURL &, int, const QPixmap &);
private:
	/** Finds the avclip that uses the given url. */
	DocClipAVFile *findAVFile(const KURL &url);
	/** A list of DocClipBase Files. There is one for each clip in the project. This is used to store
	 *  information about clips */
	DocClipBaseList m_clipList;

	/** A list of temporary clips - clips which we need to find the information of, but which we do
	 * not yet know if they should be in the project. */
	DocClipBaseList m_temporaryClipList;

	/** This renderer is for multipurpose use, such as background rendering, and for
	getting the file properties of the various AVFiles. */
	KRender * m_render;
};

#endif // CLIPMANAGER_H
