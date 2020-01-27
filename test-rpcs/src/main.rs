use std::str::FromStr;


use futures::{Future, Stream};
use hyper_openssl::openssl::ssl::{SslConnector, SslMethod};

const API_URI: &str = "https://api.mullvad.net/rpc/";
const CA_PATH: &str = "dist-assets/api_root_ca.pem";

type Client = hyper::Client<hyper_openssl::HttpsConnector<hyper::client::HttpConnector>>;


fn request(body: Vec<u8>) -> hyper::Request {
    let uri = hyper::Uri::from_str(API_URI).expect("failed to construct URI");
    let mut request = hyper::Request::new(hyper::Method::Post, uri);
    {
        let headers = request.headers_mut();
        headers.set(hyper::header::ContentType::json());
        headers.set(hyper::header::ContentLength(body.len() as u64));
    }
    request.set_body(body);
    request
}

fn relay_list_request(
    id: u64,
    client: &mut Client,
) -> Box<dyn Future<Item = hyper::Response, Error = hyper::Error>> {
    let body = format!(
        r#"{{"jsonrpc": "2.0", "method": "relay_list_v3", "id": "{}"}}"#,
        id
    );
    let req = request(body.into_bytes());
    Box::new(client.request(req))
}

fn send_requests<
    F: Fn(u64, &mut Client) -> Box<dyn Future<Item = hyper::Response, Error = hyper::Error>>,
>(
    create_future: F,
    num_requests: u64,
    client: &mut Client,
) -> impl Future<Item = (), Error = hyper::Error> {
    futures::stream::futures_unordered((0..num_requests).map(|id| {
        create_future(id, client)
            .and_then(|response| {
                response
                    .body()
                    .map(|chunk| futures::stream::iter_ok(chunk.to_vec()))
                    .flatten()
                    .collect()
            })
    }))
    .for_each(|_| Ok(()))
}

fn verify_if_should_run() {
    println!("Running this command will send multiple requests to Mullvad's API");
    println!("Enter YES to continue");
    let mut input = String::new();
    let _ = std::io::stdin().read_line(&mut input);
    if input.trim().to_lowercase() != "yes" {
        std::process::exit(1);
    }


}

fn main() {
    verify_if_should_run();

    let mut core = tokio_core::reactor::Core::new().expect("Failed to initialize tokio core");
    let mut ssl_builder = SslConnector::builder(SslMethod::tls()).expect("Failed to get ssl builder");
    ssl_builder.set_ca_file(CA_PATH).expect(  "Failed to load root CA" );


    let mut http_connector = hyper::client::HttpConnector::new(4, &core.handle());
    http_connector.enforce_http(false);
    let mut client = hyper::Client::configure()
        .keep_alive(true)
        .connector(
            hyper_openssl::HttpsConnector::with_connector(http_connector, ssl_builder).unwrap(),
        )
        .build(&core.handle());

    core.run(send_requests(relay_list_request, 100, &mut client))
        .expect("Failed to run future");

    println!("Finished, enter a line to terminate");
    let _ = std::io::stdin().read_line(&mut String::new());
}
