// Transport-agnostic bridge runtime emitted by Miro. Pair with a
// transport adapter and a command factory (e.g. makeBackend from the
// matching .backend module) to get a typed client.
//
// Transport.invoke is required; Transport.on is optional, since
// request/response transports (plain HTTP) can't push events while
// duplex transports (WebView, WebSocket) can. The Bridge result type
// surfaces `on` with whatever shape the transport provided, so users
// on event-capable transports get full typing and users on
// request/response-only transports see `on` as undefined.
//
// The optional `E` type parameter is the event map (typically the
// `ServerEvents` interface from the matching .events module). When
// supplied, `on` enforces that the event name is a declared key and
// infers the payload type. When omitted, defaults to the empty object
// type — `keyof object` is `never`, so untyped transports can't call
// `on` at all and have to opt in by parameterising explicitly.
//
// The constraint is `extends object` rather than the stricter
// `Record<string, unknown>` so interfaces can satisfy it without
// declaring an explicit index signature.

export type Invoke = (command: string, payload: unknown) => Promise<unknown>;
export type Unsubscribe = () => void;

export type TransportOn<E extends object> =
    <K extends Extract<keyof E, string>>(
        event: K,
        handler: (payload: E[K]) => void,
    ) => Unsubscribe;

export interface Transport<E extends object = object>
{
    invoke: Invoke;
    on?: TransportOn<E>;
}

export type Bridge<TBackend, E extends object = object>
    = TBackend & { on: Transport<E>['on'] };

export function makeBridge<TBackend, E extends object = object>(
    transport: Transport<E>,
    factory: (invoke: Invoke) => TBackend,
): Bridge<TBackend, E>
{
    const api = factory(transport.invoke.bind(transport)) as Bridge<TBackend, E>;
    api.on = transport.on?.bind(transport);
    return api;
}
