# routes/notifications.py

import asyncio
import json
from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import StreamingResponse
from auth_helper import get_current_user
import store

router = APIRouter(prefix="/api/notifications", tags=["Notifications"])


@router.get("")
def get_notifications(current_user: dict = Depends(get_current_user)):
    """Returns all notifications for the logged-in user, newest first."""
    user_notifications = [
        n for n in store.notifications
        if n["userId"] == current_user["id"]
    ]
    user_notifications.sort(key=lambda n: n["createdAt"], reverse=True)
    return {"notifications": user_notifications}


@router.patch("/{notification_id}/read")
def mark_read(notification_id: str, current_user: dict = Depends(get_current_user)):
    """Marks a notification as read."""
    for n in store.notifications:
        if n["id"] == notification_id and n["userId"] == current_user["id"]:
            n["read"] = True
            return {"message": "Marked as read"}
    raise HTTPException(status_code=404, detail="Notification not found")


@router.get("/stream")
async def notification_stream(current_user: dict = Depends(get_current_user)):
    """
    SSE (Server-Sent Events) stream.
    Phone connects here and keeps the connection open.
    Every time a hub sends an event, we push it here in real time.

    To test: run this curl command in Termux and then trigger a door event.
    You'll see the notification appear instantly.
    """
    user_id = current_user["id"]

    # Create a queue for this user
    queue: asyncio.Queue = asyncio.Queue()
    store.sse_queues[user_id] = queue

    async def event_generator():
        try:
            # Send a heartbeat first so client knows stream is alive
            yield "data: {\"event\": \"connected\"}\n\n"

            while True:
                try:
                    # Wait for a notification (with timeout so we can send heartbeats)
                    notification = await asyncio.wait_for(queue.get(), timeout=30)
                    payload = json.dumps(notification)
                    yield f"data: {payload}\n\n"
                except asyncio.TimeoutError:
                    # Send heartbeat every 30s to keep connection alive
                    yield "data: {\"event\": \"heartbeat\"}\n\n"
        except asyncio.CancelledError:
            pass
        finally:
            store.sse_queues.pop(user_id, None)

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",   # needed if behind nginx
        },
    )
