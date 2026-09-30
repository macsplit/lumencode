require 'sinatra'
require_relative 'lib/pricing'

get '/prices/:id' do
  pricing = Shop::Pricing.new
  pricing.total([], 0).to_s
end

post '/orders' do
  if params[:rush]
    status 202
  end
  'ok'
end
