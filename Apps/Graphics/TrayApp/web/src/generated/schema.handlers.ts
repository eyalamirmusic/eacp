import type * as T from './schema';

export type Handlers = {
    searchDownloads(req: T.SearchDownloadsRequest): T.SearchDownloadsResponse | Promise<T.SearchDownloadsResponse>;
    armDrag(req: T.ArmDragRequest): void | Promise<void>;
    copyFiles(req: T.CopyFilesRequest): T.CopyFilesResponse | Promise<T.CopyFilesResponse>;
    playAudio(req: T.PlayAudioRequest): void | Promise<void>;
    stopAudio(): void | Promise<void>;
    getPlayback(): T.PlaybackState | Promise<T.PlaybackState>;
    submitPrompt(req: T.SubmitPromptRequest): void | Promise<void>;
    dismiss(): void | Promise<void>;
};

export class UnknownCommandError extends Error
{
    httpStatus = 404;
    constructor(command: string)
    {
        super(`Unknown command: ${command}`);
    }
}

export async function dispatch(handlers: Handlers, command: string, payload: unknown): Promise<unknown>
{
    switch (command)
    {
        case 'searchDownloads': return await handlers.searchDownloads(payload as T.SearchDownloadsRequest);
        case 'armDrag': return await handlers.armDrag(payload as T.ArmDragRequest);
        case 'copyFiles': return await handlers.copyFiles(payload as T.CopyFilesRequest);
        case 'playAudio': return await handlers.playAudio(payload as T.PlayAudioRequest);
        case 'stopAudio': return await handlers.stopAudio();
        case 'getPlayback': return await handlers.getPlayback();
        case 'submitPrompt': return await handlers.submitPrompt(payload as T.SubmitPromptRequest);
        case 'dismiss': return await handlers.dismiss();
        default: throw new UnknownCommandError(command);
    }
}
