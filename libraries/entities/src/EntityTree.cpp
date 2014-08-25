//
//  EntityTree.cpp
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 12/4/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EntityTree.h"

#include "AddEntityOperator.h"
#include "DeleteEntityOperator.h"
#include "MovingEntitiesOperator.h"
#include "UpdateEntityOperator.h"

EntityTree::EntityTree(bool shouldReaverage) : Octree(shouldReaverage) {
    _rootElement = createNewElement();
}

EntityTreeElement* EntityTree::createNewElement(unsigned char * octalCode) {
    EntityTreeElement* newElement = new EntityTreeElement(octalCode);
    newElement->setTree(this);
    return newElement;
}

void EntityTree::eraseAllOctreeElements() {
    _entityToElementMap.clear();
    Octree::eraseAllOctreeElements();
}

bool EntityTree::handlesEditPacketType(PacketType packetType) const {
    // we handle these types of "edit" packets
    switch (packetType) {
        case PacketTypeEntityAddOrEdit:
        case PacketTypeEntityErase:
            return true;
        default:
            return false;
    }
}

/// Give an EntityItemID and EntityItemProperties, this will either find the correct entity that already exists
/// in the tree or it will create a new entity of the type specified by the properties and return that item.
/// In the case that it creates a new item, the item will be properly added to the tree and all appropriate lookup hashes.
EntityItem* EntityTree::getOrCreateEntityItem(const EntityItemID& entityID, const EntityItemProperties& properties) {
    EntityItem* result = NULL;

    // we need to first see if we already have the entity in our tree by finding the containing element of the entity
    EntityTreeElement* containingElement = getContainingElement(entityID);
    if (containingElement) {
        result = containingElement->getEntityWithEntityItemID(entityID);
    }
    
    // if the element does not exist, then create a new one of the specified type...
    if (!result) {
        result = addEntity(entityID, properties);
    }
    return result;
}

/// Adds a new entity item to the tree
void EntityTree::addEntityItem(EntityItem* entityItem) {
    // You should not call this on existing entities that are already part of the tree! Call updateEntity()
    EntityItemID entityID = entityItem->getEntityItemID();
    EntityTreeElement* containingElement = getContainingElement(entityID);
    assert(containingElement == NULL); // don't call addEntityItem() on existing entity items

    // Recurse the tree and store the entity in the correct tree element
    AddEntityOperator theOperator(this, entityItem);
    recurseTreeWithOperator(&theOperator);

    // check to see if we need to simulate this entity..
    changeEntityState(entityItem, EntityItem::Static, entityItem->getSimulationState());

    _isDirty = true;
}

bool EntityTree::updateEntity(const EntityItemID& entityID, const EntityItemProperties& properties) {
    // You should not call this on existing entities that are already part of the tree! Call updateEntity()
    EntityTreeElement* containingElement = getContainingElement(entityID);
    if (!containingElement) {
        //assert(containingElement); // don't call updateEntity() on entity items that don't exist
        qDebug() << "UNEXPECTED!!!!  EntityTree::updateEntity() entityID doesn't exist!!! entityID=" << entityID;
        return false;
    }

    EntityItem* existingEntity = containingElement->getEntityWithEntityItemID(entityID);
    if (!existingEntity) {
        assert(existingEntity); // don't call updateEntity() on entity items that don't exist
        return false;
    }
    
    // check to see if we need to simulate this entity...
    EntityItem::SimuationState oldState = existingEntity->getSimulationState();
    
    UpdateEntityOperator theOperator(this, containingElement, existingEntity, properties);
    recurseTreeWithOperator(&theOperator);
    _isDirty = true;

    EntityItem::SimuationState newState = existingEntity->getSimulationState();
    changeEntityState(existingEntity, oldState, newState);

    containingElement = getContainingElement(entityID);
    if (!containingElement) {
        qDebug() << "after updateEntity() we no longer have a containing element???";
        assert(containingElement); // don't call updateEntity() on entity items that don't exist
    }
    
    return true;
}


EntityItem* EntityTree::addEntity(const EntityItemID& entityID, const EntityItemProperties& properties) {

    // NOTE: This method is used in the client and the server tree. In the client, it's possible to create EntityItems 
    // that do not yet have known IDs. In the server tree however we don't want to have entities without known IDs.
    if (getIsServer() && !entityID.isKnownID) {
        //assert(entityID.isKnownID);
        qDebug() << "UNEXPECTED!!! ----- EntityTree::addEntity()... (getIsSever() && !entityID.isKnownID)";
    }

    EntityItem* result = NULL;
    // You should not call this on existing entities that are already part of the tree! Call updateEntity()
    EntityTreeElement* containingElement = getContainingElement(entityID);
    if (containingElement) {
        qDebug() << "UNEXPECTED!!! ----- EntityTree::addEntity()... entityID=" << entityID << "containingElement=" << containingElement;
        assert(containingElement == NULL); // don't call addEntity() on existing entity items
        return result;
    }

    // construct the instance of the entity
    EntityTypes::EntityType type = properties.getType();
    result = EntityTypes::constructEntityItem(type, entityID, properties);

    if (result) {
        // this does the actual adding of the entity
        addEntityItem(result);
    }
    return result;
}


void EntityTree::trackDeletedEntity(const EntityItemID& entityID) {
    // this is only needed on the server to send delete messages for recently deleted entities to the viewers
    if (getIsServer()) {
        // set up the deleted entities ID
        quint64 deletedAt = usecTimestampNow();
        _recentlyDeletedEntitiesLock.lockForWrite();
        _recentlyDeletedEntityItemIDs.insert(deletedAt, entityID.id);
        _recentlyDeletedEntitiesLock.unlock();
    }
}

void EntityTree::deleteEntity(const EntityItemID& entityID) {
    // NOTE: callers must lock the tree before using this method

    // First, look for the existing entity in the tree..
    DeleteEntityOperator theOperator(this, entityID);

    recurseTreeWithOperator(&theOperator);
    _isDirty = true;

    bool wantDebug = false;
    if (wantDebug) {
        EntityTreeElement* containingElement = getContainingElement(entityID);
        qDebug() << "EntityTree::deleteEntity().... after delete... containingElement=" << containingElement;
    }
}

void EntityTree::deleteEntities(QSet<EntityItemID> entityIDs) {
    // NOTE: callers must lock the tree before using this method
    
    const bool wantDebug = false;
    if (wantDebug) {
        qDebug() << "EntityTree::EntityTree::deleteEntities().... ";
        qDebug() << "    entityIDs=" << entityIDs;
        qDebug() << "    BEFORE map...";
        debugDumpMap();
    }
    
    DeleteEntityOperator theOperator(this);
    foreach(const EntityItemID& entityID, entityIDs) {
        // First, look for the existing entity in the tree..
        theOperator.addEntityIDToDeleteList(entityID);
    }

    recurseTreeWithOperator(&theOperator);
    _isDirty = true;

    if (wantDebug) {
        foreach(const EntityItemID& entityID, entityIDs) {
            EntityTreeElement* containingElement = getContainingElement(entityID);
            qDebug() << "EntityTree::deleteEntities().... after delete... entityID=" << entityID 
                        << "containingElement=" << containingElement;
        }

        qDebug() << "    AFTER map...";
        debugDumpMap();
    }
}

/// This method is used to find and fix entity IDs that are shifting from creator token based to known ID based entity IDs. 
/// This should only be used on a client side (viewing) tree. The typical usage is that a local editor has been creating 
/// entities in the local tree, those entities have creatorToken based entity IDs. But those entity edits are also sent up to
/// the server, and the server eventually sends back to the client two messages that can come in varying order. The first 
/// message would be a typical query/viewing data message conversation in which the viewer "sees" the newly created entity. 
/// Those entities that have been seen, will have the authoritative "known ID". Therefore there is a potential that there can 
/// be two copies of the same entity in the tree: the "local only" "creator token" version of the entity and the "seen" 
/// "knownID" version of the entity. The server also sends an "entityAdded" message to the client which contains the mapping 
/// of the creator token to the known ID. These messages can come in any order, so we need to handle the follow cases:
///
///     Case A: The local edit occurs, the addEntity message arrives, the "viewed data" has not yet arrived.
///             In this case, we can expect that our local tree has only one copy of the entity (the creator token), 
///             and we only really need to fix up that entity with a new version of the ID that includes the knownID
///
///     Case B: The local edit occurs, the "viewed data" for the new entity arrives, then the addEntity message arrives.
///             In this case, we can expect that our local tree has two copies of the entity (the creator token, and the
///             known ID version). We end up with two version of the entity because the server sends viewers only the 
///             known ID version without a creator token. And we don't yet know the mapping until we get the mapping message.
///             In this case we need to fix up that entity with a new version of the ID that includes the knownID and
///             we need to delete the extra copy of the entity.
///
/// This method handles both of these cases.
///
/// NOTE: unlike some operations on the tree, this process does not mark the tree as being changed. This is because
/// we're not changing the content of the tree, we're only changing the internal IDs that map entities from creator
/// based to known IDs. This means we don't have to recurse the tree to mark the changed path as dirty.
void EntityTree::handleAddEntityResponse(const QByteArray& packet) {

    //assert(getIsClient()); // we should only call this on client trees
    if (!getIsClient()) {
        qDebug() << "UNEXPECTED!!! EntityTree::handleAddEntityResponse() with !getIsClient() ***";
    }

    const unsigned char* dataAt = reinterpret_cast<const unsigned char*>(packet.data());
    int numBytesPacketHeader = numBytesForPacketHeader(packet);
    int bytesRead = numBytesPacketHeader;
    dataAt += numBytesPacketHeader;

    uint32_t creatorTokenID;
    memcpy(&creatorTokenID, dataAt, sizeof(creatorTokenID));
    dataAt += sizeof(creatorTokenID);
    bytesRead += sizeof(creatorTokenID);

    QUuid entityID = QUuid::fromRfc4122(packet.mid(bytesRead, NUM_BYTES_RFC4122_UUID));
    dataAt += NUM_BYTES_RFC4122_UUID;

    // First, look for the existing entity in the tree..
    EntityItemID searchEntityID;
    searchEntityID.id = entityID;
    searchEntityID.creatorTokenID = creatorTokenID;

    lockForWrite();

    // find the creator token version, it's containing element, and the entity itself    
    EntityItem* foundEntity = NULL;
    EntityItemID  creatorTokenVersion = searchEntityID.convertToCreatorTokenVersion();
    EntityItemID  knownIDVersion = searchEntityID.convertToKnownIDVersion();

    bool wantDebug = false;
    if (wantDebug) {
        qDebug() << "EntityTree::handleAddEntityResponse()..."; 
        qDebug() << "    creatorTokenID=" << creatorTokenID;
        qDebug() << "    entityID=" << entityID;
        qDebug() << "    searchEntityID=" << searchEntityID;
        qDebug() << "    creatorTokenVersion=" << creatorTokenVersion;
        qDebug() << "    knownIDVersion=" << knownIDVersion;
    }

    // First look for and find the "viewed version" of this entity... it's possible we got
    // the known ID version sent to us between us creating our local version, and getting this
    // remapping message. If this happened, we actually want to find and delete that version of
    // the entity.
    EntityTreeElement* knownIDVersionContainingElement = getContainingElement(knownIDVersion);

    if (wantDebug) {
        qDebug() << "    knownIDVersionContainingElement=" << knownIDVersionContainingElement;
    }

    if (knownIDVersionContainingElement) {
        foundEntity = knownIDVersionContainingElement->getEntityWithEntityItemID(knownIDVersion);

        if (wantDebug) {
            qDebug() << "    foundEntity=" << foundEntity;
        }

        if (foundEntity) {
            knownIDVersionContainingElement->removeEntityWithEntityItemID(knownIDVersion);
            setContainingElement(knownIDVersion, NULL);
            
            if (wantDebug) {
                qDebug() << "    FOUND VIEWED VERSION, removing entity, resetting containing element";
            }
        }
    }

    EntityTreeElement* creatorTokenContainingElement = getContainingElement(creatorTokenVersion);
    if (wantDebug) {
        qDebug() << "    creatorTokenContainingElement=" << creatorTokenContainingElement;
    }
    if (creatorTokenContainingElement) {
        foundEntity = creatorTokenContainingElement->getEntityWithEntityItemID(creatorTokenVersion);
        if (wantDebug) {
            qDebug() << "    foundEntity=" << foundEntity;
        }
        if (foundEntity) {
            creatorTokenContainingElement->updateEntityItemID(creatorTokenVersion, knownIDVersion);
            setContainingElement(creatorTokenVersion, NULL);
            setContainingElement(knownIDVersion, creatorTokenContainingElement);

            if (wantDebug) {
                qDebug() << "    FOUND CREATOR VERSION, updating entity ID and resetting containing element";
            }
        }
    }
    
    if (wantDebug) {
        qDebug() << "----MAP AFTER HANDLING RESPONSE----"; 
        debugDumpMap();
    }
    
    unlock();
}


class FindNearPointArgs {
public:
    glm::vec3 position;
    float targetRadius;
    bool found;
    const EntityItem* closestEntity;
    float closestEntityDistance;
};


bool EntityTree::findNearPointOperation(OctreeElement* element, void* extraData) {
    FindNearPointArgs* args = static_cast<FindNearPointArgs*>(extraData);
    EntityTreeElement* entityTreeElement = static_cast<EntityTreeElement*>(element);

    glm::vec3 penetration;
    bool sphereIntersection = entityTreeElement->getAACube().findSpherePenetration(args->position,
                                                                    args->targetRadius, penetration);

    // If this entityTreeElement contains the point, then search it...
    if (sphereIntersection) {
        const EntityItem* thisClosestEntity = entityTreeElement->getClosestEntity(args->position);

        // we may have gotten NULL back, meaning no entity was available
        if (thisClosestEntity) {
            glm::vec3 entityPosition = thisClosestEntity->getPosition();
            float distanceFromPointToEntity = glm::distance(entityPosition, args->position);

            // If we're within our target radius
            if (distanceFromPointToEntity <= args->targetRadius) {
                // we are closer than anything else we've found
                if (distanceFromPointToEntity < args->closestEntityDistance) {
                    args->closestEntity = thisClosestEntity;
                    args->closestEntityDistance = distanceFromPointToEntity;
                    args->found = true;
                }
            }
        }

        // we should be able to optimize this...
        return true; // keep searching in case children have closer entities
    }

    // if this element doesn't contain the point, then none of it's children can contain the point, so stop searching
    return false;
}

const EntityItem* EntityTree::findClosestEntity(glm::vec3 position, float targetRadius) {
    FindNearPointArgs args = { position, targetRadius, false, NULL, FLT_MAX };
    lockForRead();
    // NOTE: This should use recursion, since this is a spatial operation
    recurseTreeWithOperation(findNearPointOperation, &args);
    unlock();
    return args.closestEntity;
}

class FindAllNearPointArgs {
public:
    glm::vec3 position;
    float targetRadius;
    QVector<const EntityItem*> entities;
};


bool EntityTree::findInSphereOperation(OctreeElement* element, void* extraData) {
    FindAllNearPointArgs* args = static_cast<FindAllNearPointArgs*>(extraData);
    glm::vec3 penetration;
    bool sphereIntersection = element->getAACube().findSpherePenetration(args->position,
                                                                    args->targetRadius, penetration);

    // If this element contains the point, then search it...
    if (sphereIntersection) {
        EntityTreeElement* entityTreeElement = static_cast<EntityTreeElement*>(element);
        entityTreeElement->getEntities(args->position, args->targetRadius, args->entities);
        return true; // keep searching in case children have closer entities
    }

    // if this element doesn't contain the point, then none of it's children can contain the point, so stop searching
    return false;
}

// NOTE: assumes caller has handled locking
void EntityTree::findEntities(const glm::vec3& center, float radius, QVector<const EntityItem*>& foundEntities) {
    FindAllNearPointArgs args = { center, radius };
    // NOTE: This should use recursion, since this is a spatial operation
    recurseTreeWithOperation(findInSphereOperation, &args);

    // swap the two lists of entity pointers instead of copy
    foundEntities.swap(args.entities);
}

class FindEntitiesInCubeArgs {
public:
    FindEntitiesInCubeArgs(const AACube& cube) 
        : _cube(cube), _foundEntities() {
    }

    AACube _cube;
    QVector<EntityItem*> _foundEntities;
};

bool EntityTree::findInCubeOperation(OctreeElement* element, void* extraData) {
    FindEntitiesInCubeArgs* args = static_cast<FindEntitiesInCubeArgs*>(extraData);
    const AACube& elementCube = element->getAACube();
    if (elementCube.touches(args->_cube)) {
        EntityTreeElement* entityTreeElement = static_cast<EntityTreeElement*>(element);
        entityTreeElement->getEntities(args->_cube, args->_foundEntities);
        return true;
    }
    return false;
}

// NOTE: assumes caller has handled locking
void EntityTree::findEntities(const AACube& cube, QVector<EntityItem*> foundEntities) {
    FindEntitiesInCubeArgs args(cube);
    // NOTE: This should use recursion, since this is a spatial operation
    recurseTreeWithOperation(findInCubeOperation, &args);
    // swap the two lists of entity pointers instead of copy
    foundEntities.swap(args._foundEntities);
}

EntityItem* EntityTree::findEntityByID(const QUuid& id) {
    EntityItemID entityID(id);

    bool wantDebug = false;
    if (wantDebug) {
        qDebug() << "EntityTree::findEntityByID()...";
        qDebug() << "    id=" << id;
        qDebug() << "    entityID=" << entityID;
        qDebug() << "_entityToElementMap=" << _entityToElementMap;
    }

    return findEntityByEntityItemID(entityID);
}

EntityItem* EntityTree::findEntityByEntityItemID(const EntityItemID& entityID) /*const*/ {
    EntityItem* foundEntity = NULL;
    EntityTreeElement* containingElement = getContainingElement(entityID);
    if (containingElement) {
        foundEntity = containingElement->getEntityWithEntityItemID(entityID);
    }
    return foundEntity;
}

EntityItemID EntityTree::assignEntityID(const EntityItemID& entityItemID) {
    assert(getIsServer()); // NOTE: this only operates on an server tree.
    assert(!getContainingElement(entityItemID)); // NOTE: don't call this for existing entityIDs

    // The EntityItemID is responsible for assigning actual IDs and keeping track of them.
    return entityItemID.assignActualIDForToken();
}

int EntityTree::processEditPacketData(PacketType packetType, const unsigned char* packetData, int packetLength,
                    const unsigned char* editData, int maxLength, const SharedNodePointer& senderNode) {

    assert(getIsServer()); // NOTE: this only operates on an server tree.

    int processedBytes = 0;
    // we handle these types of "edit" packets
    switch (packetType) {
        case PacketTypeEntityErase: {
            const bool wantDebug = false;
            if (wantDebug) {
                qDebug() << "EntityTree::processEditPacketData().... ";
                qDebug() << "    PacketTypeEntityErase ******";
                qDebug() << "    packetLength=" << packetLength;
                qDebug() << "    maxLength=" << maxLength;

                QDebug debug = qDebug();
                debug << "       edit data contents:";
                outputBufferBits(editData, maxLength, &debug);
            }

            QByteArray dataByteArray((const char*)editData, maxLength);
            processedBytes = processEraseMessageDetails(dataByteArray, senderNode);
            break;
        }
        
        case PacketTypeEntityAddOrEdit: {
            const bool wantDebug = false;
            
            EntityItemID entityItemID;
            EntityItemProperties properties;
            
            bool validEditPacket = EntityItemProperties::decodeEntityEditPacket(editData, maxLength,
                                                    processedBytes, entityItemID, properties);

            if (wantDebug) {
                qDebug() << "EntityTree::processEditPacketData().... ";
                qDebug() << "    validEditPacket=" << validEditPacket;
                qDebug() << "    processedBytes=" << processedBytes;
                qDebug() << "    entityItemID=" << entityItemID;
                qDebug() << "    BEFORE map...";
                debugDumpMap();
            }
            
            // If we got a valid edit packet, then it could be a new entity or it could be an update to
            // an existing entity... handle appropriately
            if (validEditPacket) {
            
                // If this is a knownID, then it should exist in our tree
                if (entityItemID.isKnownID) {
                    // search for the entity by EntityItemID
                    EntityItem* existingEntity = findEntityByEntityItemID(entityItemID);
                    
                    // if the entityItem exists, then update it
                    if (existingEntity) {
                        updateEntity(entityItemID, properties);
                    } else {
                        qDebug() << "User attempted to edit an unknown entity.";
                    }
                } else {
                    // this is a new entity... assign a new entityID
                    if (wantDebug) {
                        qDebug() << "EntityTree::processEditPacketData() ... "
                                        "BEFORE assignEntityID()... entityItemID=" << entityItemID;
                    }
                    entityItemID = assignEntityID(entityItemID);
                    if (wantDebug) {
                        qDebug() << "EntityTree::processEditPacketData() ... "
                                        "AFTER assignEntityID()... entityItemID=" << entityItemID;
                    }
                    
                    EntityItem* newEntity = addEntity(entityItemID, properties);
                    if (newEntity) {
                        notifyNewlyCreatedEntity(*newEntity, senderNode);
                    }
                }
            }
            break;
        }

        default:
            processedBytes = 0;
            break;
    }
    
    return processedBytes;
}


void EntityTree::notifyNewlyCreatedEntity(const EntityItem& newEntity, const SharedNodePointer& senderNode) {
    _newlyCreatedHooksLock.lockForRead();
    for (size_t i = 0; i < _newlyCreatedHooks.size(); i++) {
        _newlyCreatedHooks[i]->entityCreated(newEntity, senderNode);
    }
    _newlyCreatedHooksLock.unlock();
}

void EntityTree::addNewlyCreatedHook(NewlyCreatedEntityHook* hook) {
    _newlyCreatedHooksLock.lockForWrite();
    _newlyCreatedHooks.push_back(hook);
    _newlyCreatedHooksLock.unlock();
}

void EntityTree::removeNewlyCreatedHook(NewlyCreatedEntityHook* hook) {
    _newlyCreatedHooksLock.lockForWrite();
    for (size_t i = 0; i < _newlyCreatedHooks.size(); i++) {
        if (_newlyCreatedHooks[i] == hook) {
            _newlyCreatedHooks.erase(_newlyCreatedHooks.begin() + i);
            break;
        }
    }
    _newlyCreatedHooksLock.unlock();
}


void EntityTree::changeEntityState(EntityItem* const entity, EntityItem::SimuationState oldState, EntityItem::SimuationState newState) {

    bool wantDebug = false;
    
    if (wantDebug) {
        qDebug() << "EntityTree::changeEntityState()....";
        qDebug() << "     oldState:" << oldState;
        qDebug() << "     newState:" << newState;

        qDebug() << "EntityTree::changeEntityState() BEFORE....";
        qDebug() << "     _changingEntities:" << _changingEntities;
        qDebug() << "     _movingEntities:" << _movingEntities;
    }

    // TODO: can we short circuit this if the state isn't changing?
    switch (oldState) {
        case EntityItem::Changing:
            _changingEntities.removeAll(entity);
            break;

        case EntityItem::Moving:
            _movingEntities.removeAll(entity);
            break;
        default:
            break;
    }


    switch (newState) {
        case EntityItem::Changing:
            _changingEntities.push_back(entity);
            break;

        case EntityItem::Moving:
            _movingEntities.push_back(entity);
            break;
        default:
            break;
    }


    if (wantDebug) {
        qDebug() << "EntityTree::changeEntityState() AFTER....";
        qDebug() << "     _changingEntities:" << _changingEntities;
        qDebug() << "     _movingEntities:" << _movingEntities;
    }

}

void EntityTree::update() {

    bool wantDebug = false;

    // our new strategy should be to segregate entities into three classes:
    //   1) stationary things that are not changing - most models
    //   2) stationary things that are animating - they can be touched linearly and they don't change the tree
    //   3) moving things - these need to scan the tree and update accordingly

    lockForWrite();
    quint64 now = usecTimestampNow();

    for (int i = 0; i < _changingEntities.size(); i++) {
        EntityItem* thisEntity = _changingEntities[i];
        thisEntity->update(now);
    }
    
    MovingEntitiesOperator moveOperator(this);
    
    QSet<EntityItem*> entitiesBecomingStatic;
    QSet<EntityItem*> entitiesBecomingChanging;

    for (int i = 0; i < _movingEntities.size(); i++) {
        EntityItem* thisEntity = _movingEntities[i];
        AACube oldCube = thisEntity->getAACube();
        thisEntity->update(now);
        AACube newCube = thisEntity->getAACube();

        if (wantDebug) {
            qDebug() << "EntityTree::update() thisEntity=" << thisEntity;
            qDebug() << "   oldCube=" << oldCube;
            qDebug() << "   newCube=" << newCube;
        }
                
        moveOperator.addEntityToMoveList(thisEntity, oldCube, newCube);

        // check to see if this entity is no longer moving
        EntityItem::SimuationState newState = thisEntity->getSimulationState();
        if (newState == EntityItem::Changing) {
            entitiesBecomingChanging << thisEntity;
        } else if (newState == EntityItem::Static) {
            entitiesBecomingStatic << thisEntity;
        }
    }
    recurseTreeWithOperator(&moveOperator);

    // for any and all entities that were moving but are now either static or changing
    // change their state accordingly
    foreach(EntityItem* entity, entitiesBecomingStatic) {
        changeEntityState(entity, EntityItem::Moving, EntityItem::Static);
    }
    foreach(EntityItem* entity, entitiesBecomingChanging) {
        changeEntityState(entity, EntityItem::Moving, EntityItem::Changing);
    }

    unlock();
}


bool EntityTree::hasEntitiesDeletedSince(quint64 sinceTime) {
    // we can probably leverage the ordered nature of QMultiMap to do this quickly...
    bool hasSomethingNewer = false;

    _recentlyDeletedEntitiesLock.lockForRead();
    QMultiMap<quint64, QUuid>::const_iterator iterator = _recentlyDeletedEntityItemIDs.constBegin();
    while (iterator != _recentlyDeletedEntityItemIDs.constEnd()) {
        //qDebug() << "considering... time/key:" << iterator.key();
        if (iterator.key() > sinceTime) {
            //qDebug() << "YES newer... time/key:" << iterator.key();
            hasSomethingNewer = true;
        }
        ++iterator;
    }
    _recentlyDeletedEntitiesLock.unlock();
    return hasSomethingNewer;
}

// sinceTime is an in/out parameter - it will be side effected with the last time sent out
bool EntityTree::encodeEntitiesDeletedSince(OCTREE_PACKET_SEQUENCE sequenceNumber, quint64& sinceTime, unsigned char* outputBuffer,
                                            size_t maxLength, size_t& outputLength) {

    bool hasMoreToSend = true;

    unsigned char* copyAt = outputBuffer;
    size_t numBytesPacketHeader = populatePacketHeader(reinterpret_cast<char*>(outputBuffer), PacketTypeEntityErase);
    copyAt += numBytesPacketHeader;
    outputLength = numBytesPacketHeader;

    // pack in flags
    OCTREE_PACKET_FLAGS flags = 0;
    OCTREE_PACKET_FLAGS* flagsAt = (OCTREE_PACKET_FLAGS*)copyAt;
    *flagsAt = flags;
    copyAt += sizeof(OCTREE_PACKET_FLAGS);
    outputLength += sizeof(OCTREE_PACKET_FLAGS);

    // pack in sequence number
    OCTREE_PACKET_SEQUENCE* sequenceAt = (OCTREE_PACKET_SEQUENCE*)copyAt;
    *sequenceAt = sequenceNumber;
    copyAt += sizeof(OCTREE_PACKET_SEQUENCE);
    outputLength += sizeof(OCTREE_PACKET_SEQUENCE);

    // pack in timestamp
    OCTREE_PACKET_SENT_TIME now = usecTimestampNow();
    OCTREE_PACKET_SENT_TIME* timeAt = (OCTREE_PACKET_SENT_TIME*)copyAt;
    *timeAt = now;
    copyAt += sizeof(OCTREE_PACKET_SENT_TIME);
    outputLength += sizeof(OCTREE_PACKET_SENT_TIME);

    uint16_t numberOfIds = 0; // placeholder for now
    unsigned char* numberOfIDsAt = copyAt;
    memcpy(copyAt, &numberOfIds, sizeof(numberOfIds));
    copyAt += sizeof(numberOfIds);
    outputLength += sizeof(numberOfIds);
    
    // we keep a multi map of entity IDs to timestamps, we only want to include the entity IDs that have been
    // deleted since we last sent to this node
    _recentlyDeletedEntitiesLock.lockForRead();
    QMultiMap<quint64, QUuid>::const_iterator iterator = _recentlyDeletedEntityItemIDs.constBegin();
    while (iterator != _recentlyDeletedEntityItemIDs.constEnd()) {
        QList<QUuid> values = _recentlyDeletedEntityItemIDs.values(iterator.key());
        for (int valueItem = 0; valueItem < values.size(); ++valueItem) {

            // if the timestamp is more recent then out last sent time, include it
            if (iterator.key() > sinceTime) {
                QUuid entityID = values.at(valueItem);
                
                QByteArray encodedEntityID = entityID.toRfc4122();
                memcpy(copyAt, encodedEntityID.constData(), NUM_BYTES_RFC4122_UUID);
                copyAt += NUM_BYTES_RFC4122_UUID;
                outputLength += NUM_BYTES_RFC4122_UUID;
                numberOfIds++;

                // check to make sure we have room for one more id...
                if (outputLength + NUM_BYTES_RFC4122_UUID > maxLength) {
                    break;
                }
            }
        }

        // check to make sure we have room for one more id...
        if (outputLength + NUM_BYTES_RFC4122_UUID > maxLength) {

            // let our caller know how far we got
            sinceTime = iterator.key();
            break;
        }
        ++iterator;
    }

    // if we got to the end, then we're done sending
    if (iterator == _recentlyDeletedEntityItemIDs.constEnd()) {
        hasMoreToSend = false;
    }
    _recentlyDeletedEntitiesLock.unlock();

    // replace the correct count for ids included
    memcpy(numberOfIDsAt, &numberOfIds, sizeof(numberOfIds));
    return hasMoreToSend;
}


// called by the server when it knows all nodes have been sent deleted packets
void EntityTree::forgetEntitiesDeletedBefore(quint64 sinceTime) {
    //qDebug() << "forgetEntitiesDeletedBefore()";
    QSet<quint64> keysToRemove;

    _recentlyDeletedEntitiesLock.lockForWrite();
    QMultiMap<quint64, QUuid>::iterator iterator = _recentlyDeletedEntityItemIDs.begin();

    // First find all the keys in the map that are older and need to be deleted
    while (iterator != _recentlyDeletedEntityItemIDs.end()) {
        if (iterator.key() <= sinceTime) {
            keysToRemove << iterator.key();
        }
        ++iterator;
    }

    // Now run through the keysToRemove and remove them
    foreach (quint64 value, keysToRemove) {
        //qDebug() << "removing the key, _recentlyDeletedEntityItemIDs.remove(value); time/key:" << value;
        _recentlyDeletedEntityItemIDs.remove(value);
    }
    
    _recentlyDeletedEntitiesLock.unlock();
}


// TODO: consider consolidating processEraseMessageDetails() and processEraseMessage()
int EntityTree::processEraseMessage(const QByteArray& dataByteArray, const SharedNodePointer& sourceNode) {
    const bool wantDebug = false;
    
    if (wantDebug) {
        qDebug() << "EntityTree::processEraseMessage()...";
    }

    const unsigned char* packetData = (const unsigned char*)dataByteArray.constData();
    const unsigned char* dataAt = packetData;
    size_t packetLength = dataByteArray.size();

    size_t numBytesPacketHeader = numBytesForPacketHeader(dataByteArray);
    size_t processedBytes = numBytesPacketHeader;
    dataAt += numBytesPacketHeader;

    dataAt += sizeof(OCTREE_PACKET_FLAGS);
    dataAt += sizeof(OCTREE_PACKET_SEQUENCE);
    dataAt += sizeof(OCTREE_PACKET_SENT_TIME);

    uint16_t numberOfIds = 0; // placeholder for now
    memcpy(&numberOfIds, dataAt, sizeof(numberOfIds));
    dataAt += sizeof(numberOfIds);
    processedBytes += sizeof(numberOfIds);

    if (wantDebug) {
        qDebug() << "EntityTree::processEraseMessage().... numberOfIds=" << numberOfIds;
        qDebug() << "EntityTree::processEraseMessage().... processedBytes=" << processedBytes;
        qDebug() << "EntityTree::processEraseMessage().... packetLength=" << packetLength;
    }

    if (numberOfIds > 0) {
        QSet<EntityItemID> entityItemIDsToDelete;

        for (size_t i = 0; i < numberOfIds; i++) {

            if (processedBytes + NUM_BYTES_RFC4122_UUID > packetLength) {
                qDebug() << "EntityTree::processEraseMessage().... bailing because not enough bytes in buffer";
                break; // bail to prevent buffer overflow
            }

            QByteArray encodedID = dataByteArray.mid(processedBytes, NUM_BYTES_RFC4122_UUID);
            QUuid entityID = QUuid::fromRfc4122(encodedID);
            dataAt += encodedID.size();
            processedBytes += encodedID.size();
            
            EntityItemID entityItemID(entityID);
            entityItemIDsToDelete << entityItemID;
            if (wantDebug) {
                qDebug() << "EntityTree::processEraseMessage()... entityItemIDsToDelete << entityItemID=" << entityItemID;
            }
        }
        if (wantDebug) {
            qDebug() << "EntityTree::processEraseMessage()... deleteEntities(entityItemIDsToDelete)";
        }
        deleteEntities(entityItemIDsToDelete);
    }
    return processedBytes;
}

// This version skips over the header
// TODO: consider consolidating processEraseMessageDetails() and processEraseMessage()
int EntityTree::processEraseMessageDetails(const QByteArray& dataByteArray, const SharedNodePointer& sourceNode) {
    const bool wantDebug = false;
    if (wantDebug) {
        qDebug() << "EntityTree::processEraseMessageDetails()...";
    }

    const unsigned char* packetData = (const unsigned char*)dataByteArray.constData();
    const unsigned char* dataAt = packetData;
    size_t packetLength = dataByteArray.size();
    size_t processedBytes = 0;

    uint16_t numberOfIds = 0; // placeholder for now
    memcpy(&numberOfIds, dataAt, sizeof(numberOfIds));
    dataAt += sizeof(numberOfIds);
    processedBytes += sizeof(numberOfIds);

    if (wantDebug) {
        qDebug() << "EntityTree::processEraseMessageDetails().... numberOfIds=" << numberOfIds;
        qDebug() << "EntityTree::processEraseMessageDetails().... processedBytes=" << processedBytes;
        qDebug() << "EntityTree::processEraseMessageDetails().... packetLength=" << packetLength;
    }

    if (numberOfIds > 0) {
        QSet<EntityItemID> entityItemIDsToDelete;

        for (size_t i = 0; i < numberOfIds; i++) {


            if (processedBytes + NUM_BYTES_RFC4122_UUID > packetLength) {
                qDebug() << "EntityTree::processEraseMessageDetails().... bailing because not enough bytes in buffer";
                break; // bail to prevent buffer overflow
            }

            QByteArray encodedID = dataByteArray.mid(processedBytes, NUM_BYTES_RFC4122_UUID);
            QUuid entityID = QUuid::fromRfc4122(encodedID);
            dataAt += encodedID.size();
            processedBytes += encodedID.size();
            
            EntityItemID entityItemID(entityID);
            entityItemIDsToDelete << entityItemID;
            if (wantDebug) {
                qDebug() << "EntityTree::processEraseMessageDetails() entityItemIDsToDelete << entityItemID=" << entityItemID;
            }
        }
        if (wantDebug) {
            qDebug() << "EntityTree::processEraseMessageDetails()... deleteEntities(entityItemIDsToDelete)";
        }
        deleteEntities(entityItemIDsToDelete);
    }
    
    return processedBytes;
}


EntityTreeElement* EntityTree::getContainingElement(const EntityItemID& entityItemID)  /*const*/ {
    bool wantDebug = false;
    
    if (wantDebug) {
        qDebug() << "EntityTree::getContainingElement() entityItemID=" << entityItemID;
        debugDumpMap();
    }

    // TODO: do we need to make this thread safe? Or is it acceptable as is
    if (_entityToElementMap.contains(entityItemID)) {
        return _entityToElementMap.value(entityItemID);
    } else if (entityItemID.creatorTokenID != UNKNOWN_ENTITY_TOKEN){
        // check the creator token version too...
        if (wantDebug) {
            qDebug() << "EntityTree::getContainingElement() checking the creator token...";
        }
        
        EntityItemID creatorTokenOnly;
        creatorTokenOnly.id = UNKNOWN_ENTITY_ID;
        creatorTokenOnly.creatorTokenID = entityItemID.creatorTokenID;
        creatorTokenOnly.isKnownID = false;

        if (_entityToElementMap.contains(entityItemID)) {
            if (wantDebug) {
                qDebug() << "EntityTree::getContainingElement() found as creator token...";
            }
            return _entityToElementMap.value(entityItemID);
        }

    }
    return NULL;
}

// TODO: do we need to make this thread safe? Or is it acceptable as is
void EntityTree::resetContainingElement(const EntityItemID& entityItemID, EntityTreeElement* element) {
    assert(entityItemID.id != UNKNOWN_ENTITY_ID);
    assert(entityItemID.creatorTokenID != UNKNOWN_ENTITY_TOKEN);
    assert(element);
    
    // remove the old version with the creatorTokenID
    EntityItemID creatorTokenVersion;
    creatorTokenVersion.id = UNKNOWN_ENTITY_ID;
    creatorTokenVersion.isKnownID = false;
    creatorTokenVersion.creatorTokenID = entityItemID.creatorTokenID;
    _entityToElementMap.remove(creatorTokenVersion);

    // set the new version with both creator token and real ID
    _entityToElementMap[entityItemID] = element;
}

void EntityTree::setContainingElement(const EntityItemID& entityItemID, EntityTreeElement* element) {
    // TODO: do we need to make this thread safe? Or is it acceptable as is
    
    // If we're a sever side tree, we always remove the creator tokens from our map items
    EntityItemID storedEntityItemID = entityItemID;
    
    if (getIsServer()) {
        storedEntityItemID.creatorTokenID = UNKNOWN_ENTITY_TOKEN;
    }
    
    if (element) {
        _entityToElementMap[storedEntityItemID] = element;
    } else {
        _entityToElementMap.remove(storedEntityItemID);
    }

    bool wantDebug = false;

    if (wantDebug) {
        qDebug() << "setContainingElement() entityItemID=" << entityItemID 
                << "storedEntityItemID=" << storedEntityItemID << "element=" << element;
        debugDumpMap();
        //qDebug() << "AFTER _entityToElementMap=" << _entityToElementMap;
    }
}

void EntityTree::debugDumpMap() {
    qDebug() << "EntityTree::debugDumpMap() --------------------------";
    QHashIterator<EntityItemID, EntityTreeElement*> i(_entityToElementMap);
    while (i.hasNext()) {
        i.next();
        qDebug() << i.key() << ": " << i.value();
    }
    qDebug() << "-----------------------------------------------------";
}

class DebugOperator : public RecurseOctreeOperator {
public:
    virtual bool PreRecursion(OctreeElement* element);
    virtual bool PostRecursion(OctreeElement* element) { return true; };
    virtual OctreeElement* PossiblyCreateChildAt(OctreeElement* element, int childIndex) { return NULL; }
};

bool DebugOperator::PreRecursion(OctreeElement* element) {
    EntityTreeElement* entityTreeElement = static_cast<EntityTreeElement*>(element);
    entityTreeElement->debugDump();
    return true;
}

void EntityTree::dumpTree() {
    // First, look for the existing entity in the tree..
    DebugOperator theOperator;
    recurseTreeWithOperator(&theOperator);
}

void EntityTree::sendEntities(EntityEditPacketSender* packetSender, float x, float y, float z) {
    SendEntitiesOperationArgs args;
    args.packetSender = packetSender;
    args.root = glm::vec3(x, y, z);
    recurseTreeWithOperation(sendEntitiesOperation, &args);
    packetSender->releaseQueuedMessages();
}

bool EntityTree::sendEntitiesOperation(OctreeElement* element, void* extraData) {
    SendEntitiesOperationArgs* args = static_cast<SendEntitiesOperationArgs*>(extraData);
    EntityTreeElement* entityTreeElement = static_cast<EntityTreeElement*>(element);

    // TODO: implement this!!
    /**
    const QList<ModelItem>& modelList = modelTreeElement->getModels();

    for (int i = 0; i < modelList.size(); i++) {
        uint32_t creatorTokenID = ModelItem::getNextCreatorTokenID();
        ModelItemID id(NEW_MODEL, creatorTokenID, false);
        ModelItemProperties properties;
        properties.copyFromNewModelItem(modelList.at(i));
        properties.setPosition(properties.getPosition() + args->root);
        args->packetSender->queueModelEditMessage(PacketTypeModelAddOrEdit, id, properties);
    }
    **/

    return true;
}

